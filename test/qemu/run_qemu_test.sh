#!/usr/bin/env bash
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
