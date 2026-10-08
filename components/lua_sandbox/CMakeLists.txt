# Lua itself is not yet wired in as a dependency (see lua_sandbox.c).
# Once added (vendored sources or a managed component), list its include
# dirs/sources here or add an idf_component.yml.
idf_component_register(
    SRCS "lua_sandbox.c"
    INCLUDE_DIRS "include"
)
