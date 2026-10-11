#include <string.h>

#include "unity.h"

#include "esp_err.h"
#include "lua_sandbox.h"
#include "lua_sandbox_internal.h"

#define TYPICAL_MAX_MEMORY (128 * 1024)

void test_lua_sandbox_create_destroy_basic(void) {
    lua_sandbox_t *sandbox = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_create(TYPICAL_MAX_MEMORY, &sandbox));
    TEST_ASSERT_NOT_NULL(sandbox);
    lua_sandbox_destroy(sandbox);
}

void test_lua_sandbox_check_syntax_accepts_valid_source(void) {
    char err_buf[128];
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_check_syntax("return 1", err_buf, sizeof(err_buf)));
}

void test_lua_sandbox_check_syntax_rejects_invalid_source(void) {
    char err_buf[128] = {0};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, lua_sandbox_check_syntax("if x then", err_buf, sizeof(err_buf)));
    TEST_ASSERT_NOT_EQUAL(0, strlen(err_buf));
}

void test_lua_sandbox_stripped_globals_are_absent(void) {
    lua_sandbox_t *sandbox = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_create(TYPICAL_MAX_MEMORY, &sandbox));
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_load(sandbox, "dofile('x') coroutine.yield()"));

    char err_buf[128] = {0};
    uint32_t yield_ms = 0;
    esp_err_t err = lua_sandbox_resume(sandbox, NULL, 0, 10000, &yield_ms, err_buf, sizeof(err_buf));
    TEST_ASSERT_EQUAL(ESP_FAIL, err);
    TEST_ASSERT_NOT_EQUAL(0, strlen(err_buf));

    lua_sandbox_destroy(sandbox);
}

void test_lua_sandbox_os_io_debug_package_not_loaded(void) {
    lua_sandbox_t *sandbox = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_create(TYPICAL_MAX_MEMORY, &sandbox));
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_load(sandbox, "return os.time()"));

    char err_buf[128] = {0};
    uint32_t yield_ms = 0;
    esp_err_t err = lua_sandbox_resume(sandbox, NULL, 0, 10000, &yield_ms, err_buf, sizeof(err_buf));
    TEST_ASSERT_EQUAL(ESP_FAIL, err);

    lua_sandbox_destroy(sandbox);
}

void test_lua_sandbox_restricted_libraries_are_usable(void) {
    lua_sandbox_t *sandbox = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_create(TYPICAL_MAX_MEMORY, &sandbox));
    TEST_ASSERT_EQUAL(ESP_OK,
                       lua_sandbox_load(sandbox, "result = math.floor(3.7) + #(\"ab\" .. \"c\") + (table.remove({1, 2, 3}))"));

    char err_buf[128] = {0};
    uint32_t yield_ms = 0;
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_resume(sandbox, NULL, 0, 10000, &yield_ms, err_buf, sizeof(err_buf)));

    double result = 0;
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_test_get_global_number(sandbox, "result", &result));
    TEST_ASSERT_EQUAL_DOUBLE(9.0, result);  // floor(3.7)=3, #"abc"=3, table.remove(...)=3

    lua_sandbox_destroy(sandbox);
}

void test_lua_sandbox_resume_without_load_returns_invalid_state(void) {
    lua_sandbox_t *sandbox = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_create(TYPICAL_MAX_MEMORY, &sandbox));

    uint32_t yield_ms = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, lua_sandbox_resume(sandbox, NULL, 0, 10000, &yield_ms, NULL, 0));

    lua_sandbox_destroy(sandbox);
}

// Stream-driven parser shape (DESIGN.md §9.1-A): the uploaded script's body
// is the coroutine directly, `...` carries the first chunk.
void test_lua_sandbox_stream_parser_yields_and_resumes(void) {
    lua_sandbox_t *sandbox = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_create(TYPICAL_MAX_MEMORY, &sandbox));
    const char *script =
        "local buffer = ...\n"
        "while true do\n"
        "    local nl = buffer:find('\\n')\n"
        "    while not nl do\n"
        "        local chunk = coroutine.yield()\n"
        "        buffer = buffer .. chunk\n"
        "        nl = buffer:find('\\n')\n"
        "    end\n"
        "    local line = buffer:sub(1, nl - 1)\n"
        "    buffer = buffer:sub(nl + 1)\n"
        "    last_line = line\n"
        "    coroutine.yield()\n"
        "end\n";
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_load(sandbox, script));

    char err_buf[128] = {0};
    uint32_t yield_ms = 0xffffffff;
    TEST_ASSERT_EQUAL(ESP_OK,
                       lua_sandbox_resume(sandbox, (const uint8_t *)"abc", 3, 10000, &yield_ms, err_buf, sizeof(err_buf)));
    TEST_ASSERT_EQUAL(0, yield_ms);  // plain yield(), not a poll-interval number

    TEST_ASSERT_EQUAL(
        ESP_OK, lua_sandbox_resume(sandbox, (const uint8_t *)"def\nghi", 7, 10000, &yield_ms, err_buf, sizeof(err_buf)));

    char line[64] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_test_get_global_string(sandbox, "last_line", line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("abcdef", line);

    lua_sandbox_destroy(sandbox);
}

// Poll-driven parser shape (DESIGN.md §9.1-B): no incoming data, the script
// yields the number of ms until its next run.
void test_lua_sandbox_poll_parser_yield_ms(void) {
    lua_sandbox_t *sandbox = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_create(TYPICAL_MAX_MEMORY, &sandbox));
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_load(sandbox, "while true do coroutine.yield(5000) end"));

    char err_buf[128] = {0};
    uint32_t yield_ms = 0;
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_resume(sandbox, NULL, 0, 10000, &yield_ms, err_buf, sizeof(err_buf)));
    TEST_ASSERT_EQUAL(5000, yield_ms);

    lua_sandbox_destroy(sandbox);
}

void test_lua_sandbox_load_replaces_previous_coroutine(void) {
    lua_sandbox_t *sandbox = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_create(TYPICAL_MAX_MEMORY, &sandbox));

    char err_buf[128] = {0};
    uint32_t yield_ms = 0;
    double marker = 0;

    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_load(sandbox, "marker = 1\ncoroutine.yield()"));
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_resume(sandbox, NULL, 0, 10000, &yield_ms, err_buf, sizeof(err_buf)));
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_test_get_global_number(sandbox, "marker", &marker));
    TEST_ASSERT_EQUAL_DOUBLE(1.0, marker);

    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_load(sandbox, "marker = 2\ncoroutine.yield()"));
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_resume(sandbox, NULL, 0, 10000, &yield_ms, err_buf, sizeof(err_buf)));
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_test_get_global_number(sandbox, "marker", &marker));
    TEST_ASSERT_EQUAL_DOUBLE(2.0, marker);

    lua_sandbox_destroy(sandbox);
}

void test_lua_sandbox_instruction_hook_fires_on_runaway_script(void) {
    lua_sandbox_t *sandbox = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_create(TYPICAL_MAX_MEMORY, &sandbox));
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_load(sandbox, "while true do end"));

    char err_buf[128] = {0};
    uint32_t yield_ms = 0;
    esp_err_t err = lua_sandbox_resume(sandbox, NULL, 0, 1000, &yield_ms, err_buf, sizeof(err_buf));
    TEST_ASSERT_EQUAL(ESP_FAIL, err);
    TEST_ASSERT_NOT_NULL(strstr(err_buf, "instruction limit"));

    lua_sandbox_destroy(sandbox);
}

void test_lua_sandbox_memory_cap_enforced(void) {
    lua_sandbox_t *sandbox = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_create(65536, &sandbox));
    TEST_ASSERT_EQUAL(ESP_OK, lua_sandbox_load(sandbox, "local s = string.rep('x', 2000000)\ncoroutine.yield()"));

    char err_buf[128] = {0};
    uint32_t yield_ms = 0;
    esp_err_t err = lua_sandbox_resume(sandbox, NULL, 0, 1000000, &yield_ms, err_buf, sizeof(err_buf));
    TEST_ASSERT_EQUAL(ESP_FAIL, err);
    TEST_ASSERT_NOT_EQUAL(0, strlen(err_buf));

    lua_sandbox_destroy(sandbox);
}
