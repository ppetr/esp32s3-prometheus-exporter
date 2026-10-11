idf_component_register(
    SRCS "lua_sandbox.c"
    INCLUDE_DIRS "include" "."
    PRIV_REQUIRES lua
)

# espressif/lua's own CMakeLists sets LUA_32BITS=1 PRIVATE, so lua_Number/
# lua_Integer are float/int32 in the compiled library, but PRIVATE means it
# isn't propagated to us - without this, lua_sandbox.c would see
# luaconf.h's un-32BITS defaults (double/int64) and read garbage back from
# any Lua number value (lua_tonumber et al. silently mismatch the real ABI).
# espressif/lua's "port/include/luaconf.h" header-injection file that's
# meant to fix this never actually takes effect: lua.h's #include
# "luaconf.h" resolves relative to lua.h's own directory first, which finds
# the real lua/luaconf.h directly, never port/include/luaconf.h.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LUA_32BITS=1)
