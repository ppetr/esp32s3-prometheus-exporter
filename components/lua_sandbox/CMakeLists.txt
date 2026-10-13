# Copyright 2026 https://github.com/ppetr/esp32s3-prometheus-exporter
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

idf_component_register(
    SRCS "lua_sandbox.c"
    INCLUDE_DIRS "include" "."
    PRIV_REQUIRES lua metrics_store
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
