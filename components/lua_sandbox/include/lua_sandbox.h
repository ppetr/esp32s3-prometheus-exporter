#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lua_sandbox lua_sandbox_t;

// Creates a sandboxed Lua state: capped allocator, restricted library set,
// dangerous globals removed (DESIGN.md §9.2). max_memory_bytes bounds total
// allocator usage for this state.
esp_err_t lua_sandbox_create(size_t max_memory_bytes, lua_sandbox_t **out_sandbox);
void lua_sandbox_destroy(lua_sandbox_t *sandbox);

// Compiles source without running it; used for upload-time validation
// (DESIGN.md §10) before touching a live channel's script.
esp_err_t lua_sandbox_check_syntax(const char *source, char *err_buf, size_t err_buf_len);

// Loads source as the sandbox's active script, replacing any previous one.
esp_err_t lua_sandbox_load(lua_sandbox_t *sandbox, const char *source);

// Resumes the loaded script's coroutine, feeding it new bytes (stream-driven
// parsers, DESIGN.md §9.1-A) or nothing (poll-driven parsers waking on their
// own schedule, DESIGN.md §9.1-B). instruction_limit bounds a single resume
// via the LUA_MASKCOUNT hook (DESIGN.md §9.2); yield_ms_out receives the
// coroutine's requested next-run delay when it yields a poll interval, or 0
// for a stream-driven yield.
esp_err_t lua_sandbox_resume(lua_sandbox_t *sandbox, const uint8_t *data, size_t len,
                              int instruction_limit, uint32_t *yield_ms_out);

#ifdef __cplusplus
}
#endif
