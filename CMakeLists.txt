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

cmake_minimum_required(VERSION 3.16)

# Restrict the build to `main` plus its actual transitive REQUIRES, instead
# of the IDF default of configuring every component under
# $IDF_PATH/components (Bluetooth, camera, SD card, ...) when COMPONENTS is
# left unset - that default cost real clean-build time without changing the
# flashed binary (unused components were already dead-code-eliminated at
# link time).
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_usb_bridge)
