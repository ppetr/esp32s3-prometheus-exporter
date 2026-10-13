#include "lua_sandbox.h"
#include "lua_sandbox_internal.h"

#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "metrics_store.h"

// Allocator state threaded through every allocation this sandbox's Lua
// state makes, including its own bootstrap (opening libraries) - so
// max_limit must leave enough room for that before any script runs.
typedef struct {
    size_t current_used;
    size_t max_limit;
} lua_mem_control_t;

struct lua_sandbox {
    lua_State *L;    // root state: owns globals, the restricted library set
    lua_State *co;   // the active channel coroutine, or NULL if none loaded
    int co_ref;       // LUA_REGISTRYINDEX ref anchoring `co` against GC
    lua_mem_control_t mem;
};

static void *l_alloc_restricted(void *ud, void *ptr, size_t osize, size_t nsize) {
    lua_mem_control_t *mc = (lua_mem_control_t *)ud;
    if (nsize == 0) {
        if (ptr) {
            mc->current_used -= osize;
            free(ptr);
        }
        return NULL;
    }
    size_t new_total = mc->current_used + nsize - (ptr ? osize : 0);
    if (new_total > mc->max_limit) {
        return NULL;
    }
    void *new_ptr = realloc(ptr, nsize);
    if (new_ptr) {
        mc->current_used = new_total;
    }
    return new_ptr;
}

// Metric-reporting functions exposed to scripts (DESIGN.md §6, §9.2). The
// channel is an upvalue baked in at registration time (push_metric_fns()),
// not a script argument - a script has no way to report a metric under any
// channel but its own.

static int l_set_metric(lua_State *L) {
    const char *channel = lua_tostring(L, lua_upvalueindex(1));
    const char *name = luaL_checkstring(L, 1);
    lua_Number value = luaL_checknumber(L, 2);
    if (metrics_store_set(channel, name, (float)value) != ESP_OK) {
        return luaL_error(L, "set_metric('%s'): failed (bad kind, or out of metric slots)", name);
    }
    return 0;
}

static int l_add_counter(lua_State *L) {
    const char *channel = lua_tostring(L, lua_upvalueindex(1));
    const char *name = luaL_checkstring(L, 1);
    lua_Number amount = luaL_checknumber(L, 2);
    if (metrics_store_counter_add(channel, name, (double)amount) != ESP_OK) {
        return luaL_error(L, "add_counter('%s'): failed (negative amount, bad kind, or out of slots)", name);
    }
    return 0;
}

// init_histogram(name, {bucket upper bounds, strictly ascending}). Must be
// called once before observe_histogram() for a given name (metrics_store's
// own requirement) - redefines (and resets) an existing histogram's buckets.
static int l_init_histogram(lua_State *L) {
    const char *channel = lua_tostring(L, lua_upvalueindex(1));
    const char *name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    lua_Integer bucket_count = luaL_len(L, 2);
    if (bucket_count <= 0 || bucket_count > METRICS_STORE_MAX_HISTOGRAM_BUCKETS) {
        return luaL_error(L, "init_histogram('%s'): bucket count %d out of range", name, (int)bucket_count);
    }

    float bounds[METRICS_STORE_MAX_HISTOGRAM_BUCKETS];
    for (lua_Integer i = 0; i < bucket_count; i++) {
        lua_geti(L, 2, i + 1);
        bounds[i] = (float)luaL_checknumber(L, -1);
        lua_pop(L, 1);
    }

    if (metrics_store_histogram_init(channel, name, bounds, (size_t)bucket_count) != ESP_OK) {
        return luaL_error(L, "init_histogram('%s'): failed (bad bucket ordering, or names a non-histogram metric)",
                           name);
    }
    return 0;
}

static int l_observe_histogram(lua_State *L) {
    const char *channel = lua_tostring(L, lua_upvalueindex(1));
    const char *name = luaL_checkstring(L, 1);
    lua_Number value = luaL_checknumber(L, 2);
    if (metrics_store_histogram_observe(channel, name, (float)value) != ESP_OK) {
        return luaL_error(L, "observe_histogram('%s'): failed (not initialized with init_histogram() yet)", name);
    }
    return 0;
}

static void push_metric_fns(lua_State *L, const char *channel) {
    static const struct {
        const char *name;
        lua_CFunction fn;
    } kFns[] = {
        {"set_metric", l_set_metric},
        {"add_counter", l_add_counter},
        {"init_histogram", l_init_histogram},
        {"observe_histogram", l_observe_histogram},
    };
    for (size_t i = 0; i < sizeof(kFns) / sizeof(kFns[0]); i++) {
        lua_pushstring(L, channel);
        lua_pushcclosure(L, kFns[i].fn, 1);
        lua_setglobal(L, kFns[i].name);
    }
}

// Restricted library set (DESIGN.md §9.2): base, table, string, math, and
// coroutine - coroutines are pure language-level control flow with no
// OS/IO surface, and are required for the yield-driven parsers in §9.1 to
// work at all. Explicitly excluded: os, io, debug, package (and therefore
// require()).
static void setup_safe_lua_env(lua_State *L, const char *channel) {
    int load_mask = LUA_GLIBK | LUA_TABLIBK | LUA_STRLIBK | LUA_MATHLIBK | LUA_COLIBK;
    luaL_openselectedlibs(L, load_mask, 0);

    // base still exposes these; strip the ones with no place in a sandboxed
    // parser script (DESIGN.md §9.2). `load` is deliberately kept - it only
    // compiles/returns a function in this same restricted environment under
    // the same instruction hook and memory cap, so it grants no extra power.
    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");
    lua_pushnil(L);
    lua_setglobal(L, "print");
    lua_pushnil(L);
    lua_setglobal(L, "collectgarbage");

    push_metric_fns(L, channel);
}

esp_err_t lua_sandbox_create(const char *channel, size_t max_memory_bytes, lua_sandbox_t **out_sandbox) {
    if (channel == NULL || out_sandbox == NULL || max_memory_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    lua_sandbox_t *sandbox = calloc(1, sizeof(lua_sandbox_t));
    if (sandbox == NULL) {
        return ESP_ERR_NO_MEM;
    }
    sandbox->mem.current_used = 0;
    sandbox->mem.max_limit = max_memory_bytes;
    sandbox->co_ref = LUA_NOREF;

    sandbox->L = lua_newstate(l_alloc_restricted, &sandbox->mem, luaL_makeseed(NULL));
    if (sandbox->L == NULL) {
        free(sandbox);
        return ESP_ERR_NO_MEM;
    }
    setup_safe_lua_env(sandbox->L, channel);

    *out_sandbox = sandbox;
    return ESP_OK;
}

// Closes and unanchors the active coroutine, if any. Caller must not be in
// the middle of resuming it.
static void discard_coroutine(lua_sandbox_t *sandbox) {
    if (sandbox->co == NULL) {
        return;
    }
    lua_closethread(sandbox->co, sandbox->L);
    luaL_unref(sandbox->L, LUA_REGISTRYINDEX, sandbox->co_ref);
    sandbox->co = NULL;
    sandbox->co_ref = LUA_NOREF;
}

void lua_sandbox_destroy(lua_sandbox_t *sandbox) {
    if (sandbox == NULL) {
        return;
    }
    discard_coroutine(sandbox);
    if (sandbox->L != NULL) {
        lua_close(sandbox->L);
    }
    free(sandbox);
}

esp_err_t lua_sandbox_check_syntax(const char *source, char *err_buf, size_t err_buf_len) {
    if (source == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    lua_State *L = luaL_newstate();
    if (L == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    if (luaL_loadstring(L, source) != LUA_OK) {
        if (err_buf != NULL && err_buf_len > 0) {
            snprintf(err_buf, err_buf_len, "%s", lua_tostring(L, -1));
        }
        err = ESP_ERR_INVALID_ARG;
    }
    lua_close(L);
    return err;
}

esp_err_t lua_sandbox_load(lua_sandbox_t *sandbox, const char *source) {
    if (sandbox == NULL || source == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    lua_State *new_co = lua_newthread(sandbox->L);
    int new_ref = luaL_ref(sandbox->L, LUA_REGISTRYINDEX);

    if (luaL_loadbuffer(new_co, source, strlen(source), "channel_script") != LUA_OK) {
        lua_closethread(new_co, sandbox->L);
        luaL_unref(sandbox->L, LUA_REGISTRYINDEX, new_ref);
        return ESP_ERR_INVALID_ARG;
    }

    discard_coroutine(sandbox);
    sandbox->co = new_co;
    sandbox->co_ref = new_ref;
    return ESP_OK;
}

static void instruction_hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    luaL_error(L, "script exceeded its instruction limit");
}

esp_err_t lua_sandbox_resume(lua_sandbox_t *sandbox, const uint8_t *data, size_t len,
                              int instruction_limit, uint32_t *yield_ms_out,
                              char *err_buf, size_t err_buf_len) {
    if (sandbox == NULL || yield_ms_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sandbox->co == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    *yield_ms_out = 0;

    int nargs = 0;
    if (data != NULL) {
        lua_pushlstring(sandbox->co, (const char *)data, len);
        nargs = 1;
    }

    if (instruction_limit > 0) {
        lua_sethook(sandbox->co, instruction_hook, LUA_MASKCOUNT, instruction_limit);
    }
    int nres = 0;
    // "from" is NULL, not sandbox->L: sandbox->L isn't actually the thread
    // driving this resume (nothing is resuming it) - matches DESIGN.md
    // §9.1's own C-side resume example.
    int status = lua_resume(sandbox->co, NULL, nargs, &nres);
    lua_sethook(sandbox->co, NULL, 0, 0);

    esp_err_t err = ESP_OK;
    if (status == LUA_OK || status == LUA_YIELD) {
        if (nres >= 1 && lua_isnumber(sandbox->co, 1)) {
            *yield_ms_out = (uint32_t)lua_tonumber(sandbox->co, 1);
        }
    } else {
        if (err_buf != NULL && err_buf_len > 0) {
            const char *msg = lua_isstring(sandbox->co, -1) ? lua_tostring(sandbox->co, -1) : "(non-string error)";
            snprintf(err_buf, err_buf_len, "%s", msg);
        }
        err = ESP_FAIL;
    }
    lua_settop(sandbox->co, 0);
    return err;
}

esp_err_t lua_sandbox_test_get_global_number(lua_sandbox_t *sandbox, const char *name, double *out) {
    if (sandbox == NULL || name == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lua_getglobal(sandbox->L, name);
    if (!lua_isnumber(sandbox->L, -1)) {
        lua_pop(sandbox->L, 1);
        return ESP_ERR_NOT_FOUND;
    }
    *out = (double)lua_tonumber(sandbox->L, -1);
    lua_pop(sandbox->L, 1);
    return ESP_OK;
}

esp_err_t lua_sandbox_test_get_global_string(lua_sandbox_t *sandbox, const char *name, char *buf, size_t buf_len) {
    if (sandbox == NULL || name == NULL || buf == NULL || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    lua_getglobal(sandbox->L, name);
    if (!lua_isstring(sandbox->L, -1)) {
        lua_pop(sandbox->L, 1);
        return ESP_ERR_NOT_FOUND;
    }
    snprintf(buf, buf_len, "%s", lua_tostring(sandbox->L, -1));
    lua_pop(sandbox->L, 1);
    return ESP_OK;
}
