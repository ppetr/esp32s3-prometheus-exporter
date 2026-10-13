#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lua_sandbox lua_sandbox_t;

// Creates a sandboxed Lua state: capped allocator, restricted library set,
// dangerous globals removed, metric-reporting functions added (DESIGN.md
// §9.2). max_memory_bytes bounds total allocator usage for this state.
// channel is copied into the Lua state as the implicit first argument to
// every metric-reporting call (set_metric/add_counter/init_histogram/
// observe_histogram, DESIGN.md §6) - the script itself never names its own
// channel, so it can't misattribute a metric to a different one.
esp_err_t lua_sandbox_create(const char *channel, size_t max_memory_bytes, lua_sandbox_t **out_sandbox);
void lua_sandbox_destroy(lua_sandbox_t *sandbox);

// Compiles source without running it; used for upload-time validation
// (DESIGN.md §10) before touching a live channel's script. On a syntax
// error, writes Lua's own message into err_buf (if non-NULL) and returns
// ESP_ERR_INVALID_ARG.
esp_err_t lua_sandbox_check_syntax(const char *source, char *err_buf, size_t err_buf_len);

// Loads source as the sandbox's active script, replacing (and properly
// closing) any previous one. The source is the parser body itself - not
// wrapped in `coroutine.create(function(...) ... end)` as sketched in
// DESIGN.md §9.1's illustrative Lua; a top-level Lua chunk is already an
// implicit vararg function, so the body alone is enough. The first
// lua_sandbox_resume() call's data becomes this body's `...`
// (DESIGN.md §9.1-A's `first_chunk`); later calls' data becomes the result
// of whatever `coroutine.yield()` it's paused at.
esp_err_t lua_sandbox_load(lua_sandbox_t *sandbox, const char *source);

// Resumes the loaded script's coroutine, feeding it new bytes (stream-driven
// parsers, DESIGN.md §9.1-A) or nothing (poll-driven parsers waking on their
// own schedule, DESIGN.md §9.1-B: pass data=NULL). instruction_limit bounds
// a single resume via the LUA_MASKCOUNT hook (DESIGN.md §9.2) - 0 means no
// limit. *yield_ms_out receives the coroutine's requested next-run delay
// when it yields a single number (the poll-driven shape), or 0 otherwise.
//
// Returns ESP_OK if the resume itself completed without a Lua runtime
// error, whether the coroutine yielded or ran to completion - a completed
// coroutine simply can't be resumed again (the next attempt surfaces Lua's
// own "cannot resume dead coroutine" through err_buf, which is deliberately
// not special-cased here). On a runtime error (a script bug, or the
// instruction-hook firing on a runaway script), writes Lua's error message
// into err_buf (if non-NULL) and returns ESP_FAIL - intended to feed
// metrics_store_set_channel_status()'s lua_error parameter (DESIGN.md §6.3).
esp_err_t lua_sandbox_resume(lua_sandbox_t *sandbox, const uint8_t *data, size_t len,
                              int instruction_limit, uint32_t *yield_ms_out,
                              char *err_buf, size_t err_buf_len);

// Current/maximum bytes tracked by this sandbox's capped allocator
// (DESIGN.md §7) - the same accounting l_alloc_restricted() enforces
// max_memory_bytes against, read back for exporting as a Prometheus gauge.
// Return 0 for a NULL sandbox.
size_t lua_sandbox_memory_used(const lua_sandbox_t *sandbox);
size_t lua_sandbox_memory_limit(const lua_sandbox_t *sandbox);

#ifdef __cplusplus
}
#endif
