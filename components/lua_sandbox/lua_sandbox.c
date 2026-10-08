#include "lua_sandbox.h"

#include "esp_log.h"

static const char *TAG = "lua_sandbox";

// Not yet implemented: this component has no Lua dependency wired in yet
// (DESIGN.md §13 — Lua 5.4 source needs to be vendored or pulled as a
// managed component before lua.h/lauxlib.h can be used here). The allocator,
// instruction hook, and restricted-library setup from DESIGN.md §9.2 land
// in this file once that dependency exists.

struct lua_sandbox {
    size_t max_memory_bytes;
};

esp_err_t lua_sandbox_create(size_t max_memory_bytes, lua_sandbox_t **out_sandbox) {
    (void)max_memory_bytes;
    (void)out_sandbox;
    ESP_LOGW(TAG, "lua_sandbox_create: not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

void lua_sandbox_destroy(lua_sandbox_t *sandbox) {
    (void)sandbox;
}

esp_err_t lua_sandbox_check_syntax(const char *source, char *err_buf, size_t err_buf_len) {
    (void)source;
    (void)err_buf;
    (void)err_buf_len;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t lua_sandbox_load(lua_sandbox_t *sandbox, const char *source) {
    (void)sandbox;
    (void)source;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t lua_sandbox_resume(lua_sandbox_t *sandbox, const uint8_t *data, size_t len,
                              int instruction_limit, uint32_t *yield_ms_out) {
    (void)sandbox;
    (void)data;
    (void)len;
    (void)instruction_limit;
    (void)yield_ms_out;
    return ESP_ERR_NOT_SUPPORTED;
}
