#!/usr/bin/env bash

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

set -euo pipefail

# Tier 2 QEMU integration test driver (DESIGN.md §12.3).
#
# Not yet implemented: needs a QEMU-variant firmware build first (built with
# channel_source_fake wired in place of usb_host_bridge/i2c_poll_source, and
# net_eth using QEMU's emulated netif instead of the real W5500 driver — see
# the open item in DESIGN.md §13). Once that build exists, this script should:
#   1. idf.py qemu (boot the image, with the fake channel's backing file
#      pre-seeded with test bytes).
#   2. curl the exposed HTTP port(s) and assert on /metrics and /scripts/*
#      responses.

echo "test/qemu: not yet implemented, see DESIGN.md §12.3" >&2
exit 1
