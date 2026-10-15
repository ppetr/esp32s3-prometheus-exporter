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
    SRCS "test_runner.c" "test_metrics_store.c" "test_lua_sandbox.c" "test_channel_registry.c" "test_lua_pump.c"
         "test_upload_auth.c" "test_uzlib_inflate.c"
    INCLUDE_DIRS "."
    REQUIRES metrics_store lua_sandbox channel_registry lua_pump upload_auth uzlib_inflate unity
)
