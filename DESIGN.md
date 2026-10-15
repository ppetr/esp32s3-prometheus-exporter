# ESP32-S3 Telemetry Bridge — Design Document

Target audience: an implementing engineer/agent building this from scratch in
ESP-IDF. This expands on an exploratory chat about hardware selection and
architecture; it resolves the open questions from that discussion into
concrete decisions and flags what is still genuinely undecided.

## 1. Goal

A LAN-connected bridge that:

- Talks to one or more peripheral devices over USB Host (CDC-ACM serial),
  with I2C/SPI polling as a planned extension.
- Lets the end user supply a small **Lua script per channel** that parses
  raw bytes (or polls a register) into named metric values — because each
  monitored device speaks a different protocol and hand-coding a C++ parser
  per device isn't scalable.
- Exposes the extracted values as a **Prometheus** `/metrics` endpoint over
  Ethernet.

Non-goals: WiFi (Ethernet-only), battery operation, multi-tenant auth on the
upload endpoint (single-trust-domain LAN assumed unless stated otherwise).

## 2. Hardware

Board: **Waveshare ESP32-S3-ETH** (or compatible clone), chip **ESP32-S3R8**
(8 MB PSRAM, 16 MB flash), **W5500** Ethernet over SPI, add-on **PoE module**
(802.3af), native USB-C on the S3's built-in USB OTG peripheral.

Key hardware constraints to design around:

- **Single USB peripheral.** The S3 has exactly one USB controller. Once
  firmware switches it into Host mode, the USB-C port can no longer act as
  the USB-Serial/JTAG device used for flashing, the console, and the
  CIRCUITPY/USB-device link. This is not a software bug to work around — it
  is implicit in picking native USB-S3 Host over an external MAX3421E chip.
  Section 4 covers the mode-switch strategy.
- **Download mode is always available as a fallback.** Even if the app
  firmware hangs in Host mode, holding BOOT at reset puts the chip in ROM
  download mode, so flashing is never permanently blocked by a bad build.
- **W5500 pinout (confirmed from the official Waveshare wiki, not
  assumed):** SCLK 13, MOSI 11, MISO 12, CS 14, INT 10, RST 9, on its own
  SPI bus separate from any general-purpose SPI exposed for sensors —
  sharing a bus with Ethernet would add SPI-arbitration concerns between
  Core 0 (Ethernet) and Core 1 (sensor polling). All six are
  `net_eth`-Kconfig options (`CONFIG_NET_ETH_SPI_*_GPIO` etc., see
  `components/net_eth/Kconfig`), so a different board/wiring is a
  `menuconfig` change, not a code change.
- **PoE power budget.** 802.3af typically yields ~6–12 W after the PoE
  module's conversion losses. Budget for the ESP32 + W5500 + any
  USB-bus-powered client device. Expose the regulator's 5 V-rail voltage on
  an ADC-monitored metric (see §7) as a diagnostic — a sagging rail under
  USB inrush is the most likely field failure mode.
- **AM2302 (wired DHT22) sensor**, the project's first real `channel_source`
  driver (§9.1): `OUT` wired to GPIO1 (`CONFIG_DHT22_SOURCE_GPIO`) - not a
  boot-strapping pin, and free of the W5500's SPI bus. Powered from
  **3.3V, not 5V** - at 5V its pulled-up `OUT` line would idle above the
  GPIO's absolute maximum input voltage.

## 3. Toolchain and project layout

- Build system: **ESP-IDF** (CMake + Ninja under the hood). No Arduino
  layer — needed for first-class USB Host CDC-ACM support and finer control
  over task placement. Language is C++, not Rust/Nim/Zig — see §14 for why.
- Editor: Neovim + `clangd`. Run `idf.py reconfigure` (or any `idf.py`
  build) to (re)generate `build/compile_commands.json`; point clangd at it.
- Wrap the two commands actually used day to day in a `Makefile` at the
  repo root so the workflow is `make build` / `make upload`:

  ```makefile
  build:
  	idf.py build

  upload:
  	idf.py flash monitor
  ```

- Project layout: one ESP-IDF **component per subsystem**, each with its
  own `CMakeLists.txt`, so pieces are independently testable/reusable. Driver
  components implement a narrow `channel_source` interface (§13) rather than
  being called directly from scheduling/parsing code, so a fake
  implementation can stand in for them off-hardware:

  ```
  components/
    channel_source/      # the interface itself (header-only) + a fake/file-backed impl used by tests
    drivers/              # real channel_source implementations, grouped (one per sensor/bus kind)
      usb_host_bridge/    # USB Host CDC-ACM task, ring buffer handoff
      i2c_poll_source/    # I2C register polling
      dht22_source/        # AM2302/DHT22 single-wire bit-banged read (§9.1)
    lua_sandbox/          # lua_State setup, allocator, hooks, safe API — hardware-independent
    channel_registry/     # channel name -> lua_sandbox_t* map + scheduling (§9.1-§9.2) — hardware-independent
    lua_pump/              # the Lua pump task itself (§5, §9.1) — step logic hardware-independent, task creation Tier 2/3
    metrics_store/        # metric map + staleness logic — hardware-independent
    http_api/              # /metrics and /scripts endpoints — hardware-independent except for the httpd dependency
    self_doc/              # /, /manual.md, /openapi.yaml (§11.1) — build-time render+compress, httpd at runtime
    uzlib_inflate/         # vendored raw-DEFLATE streaming decoder (§11.1) — hardware-independent
    net_eth/               # W5500 init, SNTP — real hardware only
  docs/
    manual.md              # the operator's manual itself (§11.1) — not this document
    openapi.yaml           # OpenAPI 3.x spec for the HTTP API (§11.1)
  main/
    app_main.cpp          # task creation, core pinning, wiring components together
  test/
    host/                 # Tier 1: native Linux-target unit tests (Unity), no ESP-IDF hardware init at all
    qemu/                 # Tier 2: full firmware image + scripts driving it under `idf.py qemu`
  ```

## 4. USB mode switching (device-at-boot, host-in-app)

Flashing and the console need USB **device** mode; the running application
needs USB **host** mode. Since only one can be active, resolve it at boot:

1. `boot.py`-equivalent for C++: before `app_main` initializes the USB Host
   stack, read a dedicated GPIO (pulled up internally, grounded by a jumper
   or button to select "maintenance mode").
2. If the pin is asserted at boot: skip USB Host init entirely, stay on
   USB-Serial/JTAG, blink an LED to confirm maintenance mode is active, and
   just idle (or run Ethernet/HTTP only, for OTA-style script upload without
   needing USB Host at all).
3. If not asserted: proceed to init USB Host and hand the port to the CDC
   bridge. The port will no longer enumerate as a serial device to a host
   PC from this point on.
4. Recovery path if a bad build gets stuck before it reads the pin at all:
   BOOT-button download mode always works regardless of application state,
   since it's handled by ROM bootloader, not application code.

GPIO choice, exact pull direction, and whether the jumper is normally-open
or normally-closed are **not yet decided** — pick a spare, boot-safe-strap
pin per the Waveshare schematic and document it in code at the point of use.

## 5. Task and core layout

FreeRTOS SMP; pin tasks explicitly (`xTaskCreatePinnedToCore`) rather than
letting the scheduler migrate them, so Ethernet/WiFi-stack jitter never
steals cycles from USB servicing:

| Task | Core | Priority | Responsibility |
|---|---|---|---|
| LwIP / Ethernet (W5500) | 0 | system default | network stack, driven by ESP-IDF |
| HTTP server | 0 | medium | `/metrics`, `/scripts/*` — reads snapshot only, never touches USB/Lua |
| USB Host CDC task | 1 | high | blocking read on `usb_host_cdc_acm_receive`-style API, pushes raw bytes into a ring buffer, never calls into Lua directly |
| Lua pump / scheduler task | 1 | medium | drains the ring buffer, resumes the relevant coroutine; also drives time-based coroutines (I2C polling) |
| (future) I2C/SPI polling | 1 | low/medium | only if implemented as a dedicated task rather than folded into the Lua pump's scheduler loop |
| DHT22 poll task | 1 | low | bit-bangs the AM2302 single-wire protocol every ~2.5s, delivers the raw 5-byte reading via `channel_registry_push_data()`, the same mechanism `usb_read()` uses internally (§9.1) |

Rationale for splitting the USB read task from the Lua pump: the USB task
must never block on Lua (a slow parser or GC pause must not risk a NAK
storm or dropped bytes). The ring buffer absorbs the mismatch — see §8 for
why the margin is large in practice.

Enable the Task Watchdog on the USB task and the Lua pump task. A Lua
script that somehow escapes the instruction hook (bug in the hook itself,
not just a runaway user script) should still trip the watchdog and reset
rather than silently wedge the bridge.

**Implemented**: the Lua pump task (`lua_pump_start()`) registers with
the Task Watchdog on a best-effort basis (skipped at compile time if
`CONFIG_ESP_TASK_WDT_EN` isn't set, which it isn't on the Linux target's
default config - not fatal either way, since the watchdog is a hardware
safety net, not load-bearing for Tier 1). Its priority
(`tskIDLE_PRIORITY + 10`) is a placeholder, not yet tuned against the
other tasks in this table once they exist. "The ring buffer" the USB task
would push into is, for now, `channel_registry`'s fixed per-channel
pending-data buffer (§9.1) rather than a true ring buffer - non-blocking,
drop-on-full, sized to §8's suggested range; swap in a real ring buffer
later if throughput ever demands it.

**Implemented**: `net_eth_init()` brings up the W5500 (`espressif/w5500`
managed component), DHCP, and best-effort SNTP (§6.4); confirmed on real
hardware - link up, DHCP lease obtained, host reachable by ping. It runs
entirely on ESP-IDF's own Ethernet/LwIP tasks (no task of ours to pin). Not
yet done: a dedicated diagnostics metric exposing link state (§7).

**Implemented**: `dht22_source_init()` registers the `"dht22"` channel and
starts the DHT22 poll task above (Core 1, pinned). Confirmed against real
AM2302 hardware - see §9.1 for why its own timing means the driver polls
independently and delivers bytes to the channel rather than the Lua
script calling a read function on its own schedule, and
`tools/scripts/dht22.lua` for the counterpart script to upload.

## 6. Metrics storage and the Prometheus exporter

### 6.1 Data model

Each channel (one per USB device / future I2C sensor) owns a small set of
named metrics. Store as a map keyed by metric name, value = `{float value,
uint32_t last_update_tick}` (`last_update_tick` from `xTaskGetTickCount()`
or `esp_timer_get_time()`, not wall-clock — see §6.3). A metric slot is
independently a gauge (as above), a counter (§6.6), or a histogram
(§6.5) — the split between the three isn't known ahead of time, so they
share one pool of slots per channel rather than having separate
per-kind limits.

### 6.2 Cross-core handoff

Two viable options, pick one rather than mixing both per metric:

- **Mutex/Read-write-lock-protected map** (simplest, recommended default):
  Core 1 locks, updates one or more entries, unlocks. Core 0's HTTP handler
  locks, copies/serializes, unlocks. Fine as long as the critical section
  stays short (just struct field writes, no Lua calls while holding the
  lock).
- **Double-buffer + atomic pointer swap**: Core 1 builds a whole new
  snapshot struct (e.g. in PSRAM, from a small static pool of 2–3
  preallocated buffers to avoid heap churn/fragmentation), then
  `std::atomic<Snapshot*>::exchange` publishes it; Core 0 just `.load()`s
  the current pointer. Removes any lock contention on the HTTP path at the
  cost of copying the whole metric set per update instead of touching one
  field. Prefer this only if profiling shows mutex contention actually
  matters — for a handful of metrics updated at human-observable rates, it
  won't.

Either way: the HTTP handler **never** touches USB, I2C, or the Lua
interpreter synchronously. It only reads whatever the collection side most
recently published. This was an explicit design decision in the original
discussion: Prometheus scrape timeouts are typically ~10 s, but a well
behaved exporter responds in the low hundreds of milliseconds, and tying
the HTTP response to live I/O risks exhausting the HTTP server's small
connection pool (ESP-IDF's httpd defaults to ~4–8 concurrent sockets) and
produces user-visible scrape gaps whenever the parser happens to be slow.

### 6.3 Staleness, not embedded timestamps

Do not put a `*_last_seen_seconds` metric next to each value as a generic
mechanism — the original plan — because:

- It doubles the series count and requires a PromQL join (`on()` /
  `group_left()`) to use, which is awkward for the common case.
- A timestamp as a **label** is explicitly wrong (cardinality explosion —
  every update mints a new series).

Instead:

1. Track `last_update_tick` per metric internally (monotonic tick count,
   survives not having NTP).
2. In the `/metrics` handler, compute `now - last_update_tick`. If it
   exceeds a staleness threshold, **omit that metric line entirely** for
   this scrape. Prometheus's own staleness handling (a missing series after
   having seen it) then naturally produces the gap in Grafana — no custom
   convention needed on the query side.
3. Always emit one small, always-present health metric per channel
   regardless of staleness, e.g.:
   `usb_bridge_channel_info{channel="dev0",status="online"} 1` — flip
   `status` (or add a `lua_error` label) when the channel's last Lua
   execution failed or the device hasn't produced data recently. This is
   the one series a dashboard/alert can always rely on existing.
4. **Staleness threshold value is not yet decided** — a reasonable default
   is "2× the channel's own observed update interval", but that requires
   tracking the interval too; a simpler fixed default (e.g. 60 s) is
   acceptable for v1 and should be a per-channel constant, not a
   user-facing config knob (see §9 — avoiding config surface was an
   explicit goal).
5. Never substitute `0` for "no data" — `0` is frequently a valid sensor
   reading.

### 6.4 Absolute time (optional, best-effort)

Run SNTP (`esp_sntp_setoperatingmode` + `pool.ntp.org` or a LAN NTP server)
as an independent background concern, not gating anything on it:

- Maintain a `bool time_synchronized` flag, set once SNTP completes.
- If synchronized, the exporter may append a real Unix timestamp to metric
  lines; if not, omit it and let Prometheus use scrape-time — this is valid
  Prometheus exposition format either way, so there's no hard dependency on
  NTP succeeding.
- This keeps the bridge fully functional on a closed network with no
  Internet access, at the cost of losing absolute-time precision (falls
  back to Prometheus's own scrape timestamp, which is normally good
  enough).

### 6.5 Histogram-shaped metrics

Not every channel's data is a single scalar — a few scripts will want a
Prometheus histogram (e.g. a parse-latency or frame-size distribution)
rather than a gauge. This is a metric slot *kind*, not a separate concept:

- `metrics_store_histogram_init(channel, name, bucket_upper_bounds,
  bucket_count)` defines (or redefines, resetting counts) a histogram's
  bucket boundaries, which must be strictly ascending; an implicit `+Inf`
  bucket catches every observation, as in the standard Prometheus client
  libraries.
- `metrics_store_histogram_observe(channel, name, value)` increments the
  matching bucket plus the running count/sum. Returns "not found" if
  `histogram_init` wasn't called first — there's no implicit creation on
  first observe the way plain `metrics_store_set` creates a gauge, since
  the bucket boundaries have to come from somewhere.
- Bucket storage (`float` bounds + `uint32_t` counts) is **heap-allocated
  once**, in `histogram_init`, sized exactly to the caller's
  `bucket_count` — never reallocated on the `observe` hot path. This was a
  deliberate middle ground from the original discussion: fully static
  arrays would force one fixed bucket count for every histogram ever
  defined (wasteful, or too small for someone's legitimate use), while a
  general-purpose dynamic container (e.g. `absl::btree_map`) buys nothing
  at this scale (single-digit channels, a handful of metrics each) and
  reintroduces the heap-churn concern §6.2 already avoids for the
  gauge/channel maps — not to mention being unproven on the Xtensa
  toolchain, the same category of risk that ruled out Rust in §14.
- `METRICS_STORE_MAX_HISTOGRAM_BUCKETS` (128) is a sanity ceiling on the
  one-time allocation request, not a fixed array size — it exists so a bad
  `/scripts/*` upload can't make the firmware allocate an unbounded amount
  of memory.
- Exposition format matches the standard Prometheus histogram shape:
  `name_bucket{channel="...",le="..."} <cumulative count>` for each
  boundary plus `+Inf`, then `name_sum{channel="..."} <sum>` and
  `name_count{channel="..."} <count>`. Staleness (§6.3) applies to the
  whole histogram at once, keyed off the last `observe` call.
- **Not yet decided**: whether `metrics_store_histogram_init`'s one-time
  allocation should be steered to PSRAM (`heap_caps_malloc` with
  `MALLOC_CAP_SPIRAM`) rather than plain `malloc()`/internal SRAM — not a
  concern at the bucket counts scripts will realistically use, but worth
  revisiting if a script ever defines many large histograms at once (see
  §13).

### 6.6 Counter metrics

A monotonically increasing count of events (bytes received, parse errors,
...) is common enough to warrant its own kind rather than making every
script re-implement "gauge that I only ever increase":

- `metrics_store_counter_add(channel, name, amount)` creates the entry at 0
  on first use (like `metrics_store_set` does for gauges — no separate
  `_init` step needed, unlike histograms, since there's no configuration
  to supply up front) and rejects a negative `amount`, since a counter
  going down is specifically what distinguishes it from a gauge. A process
  restart resets it to 0 - this is fine as-is because Prometheus's
  `rate()`/`increase()` already treat any decrease as a reset.
- Formatted the same as a gauge (`name{channel="..."} value`); by
  Prometheus convention the caller should give the metric itself a
  trailing `_total` in its name (e.g. `"bytes_received_total"`) - this
  function doesn't add the suffix, matching how gauge/histogram also use
  the name verbatim.
- Same type-mismatch guard as gauge/histogram: once a name is a counter on
  a channel, calling `metrics_store_set`/`metrics_store_histogram_init` on
  that same name returns `ESP_ERR_INVALID_STATE`, and vice versa.

## 7. Diagnostics metric

Expose the PoE 5 V-rail voltage via ADC (if the board breaks it out to a
divider) as a gauge, e.g. `esp32_poe_5v_rail_volts`. This is cheap to add
and is the most likely early warning for USB-inrush-induced brownouts. **Not
yet done** - needs the PoE module physically connected first.

**Implemented** (process-wide diagnostics, no `channel` label - same
unlabeled shape as the uptime gauge in §6.4, since none of these are scoped
to one channel):

- `esp32_usb_bridge_heap_free_bytes` / `..._heap_total_bytes` and
  `..._psram_free_bytes` / `..._psram_total_bytes` (internal SRAM vs. octal
  PSRAM, via `heap_caps_get_free/total_size(MALLOC_CAP_INTERNAL|SPIRAM)`).
- `esp32_usb_bridge_cpu_usage_percent{core="0"|"1"}` - each core's non-idle
  fraction since the *previous* scrape (not a since-boot average), derived
  from `uxTaskGetSystemState()`'s per-task run-time counters
  (`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y`). Needs two samples, so it's
  absent from the very first `/metrics` response after boot.
- `esp32_usb_bridge_eth_link_up` (0/1), plus `..._eth_link_speed_mbps` and
  `..._eth_full_duplex` while up - pushed by `net_eth`'s own link event
  handler (`ETH_CMD_G_SPEED`/`ETH_CMD_G_DUPLEX_MODE`), not polled.
- `esp32_usb_bridge_task_stack_free_bytes{task="..."}` - one line per
  FreeRTOS task, from the same `uxTaskGetSystemState()` snapshot the CPU
  gauge uses (`usStackHighWaterMark`, already in bytes on this port).
- `esp32_usb_bridge_chip_temp_celsius` - the S3's internal die temperature
  sensor (`driver/temperature_sensor.h`, installed lazily on first scrape,
  `TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50)` - this range lands on the
  chip's most accurate calibrated band, -10..80 °C at ±1 °C, same as
  ESP-IDF's own example). Omitted if install/enable ever fails (no retry
  after that). The S3 has no other usable built-in sensor: no Hall sensor
  (removed after the original ESP32), and the touch-sensor peripheral it
  does have needs an external pad to mean anything.

All of the above (except the Ethernet link gauges, which net_eth pushes on
its own events) are collected by a new `sys_metrics` component and refreshed
once per `/metrics` scrape (`sys_metrics_refresh()`, called from
`http_api`'s handler right before `metrics_store_format()`) - not polled on
a timer, since nothing else needs this data between scrapes.

Also **implemented**, per-channel (uses the ordinary `metrics_store_set()`
gauge path, so it does carry a `channel` label like any script-reported
metric): `lua_memory_used_bytes` / `lua_memory_limit_bytes`, reporting each
channel's Lua sandbox allocator usage (`lua_sandbox_memory_used/_limit()`,
reading the same counter the capped allocator enforces §9.2's memory limit
against). Refreshed by `lua_pump`'s `report_result()` after every resume,
so it goes stale (and is omitted, §6.3) for a channel that stops being
resumed.

## 8. USB Host read path and flow control

- USB on ESP32-S3 is host-initiated at the protocol level: if the CDC task
  falls behind draining the hardware FIFO, the host hardware answers the
  client with NAK automatically — the client buffers and retries, so a
  momentarily slow consumer does not silently lose bytes. This is a
  property of the USB protocol/stack, not something the application needs
  to implement.
- At typical serial rates (115200 baud ≈ one byte per ~86 µs) against a
  stack-level buffer in the 256–512 byte range, the task has on the order
  of tens of milliseconds of slack before anything would need to apply
  back-pressure. A Lua `pcall` is tens of microseconds; an occasional GC
  pause is low single-digit milliseconds. Net: no explicit rate-limiting of
  the USB read task is needed for the data rates in scope here.
- Set a receive timeout (e.g. 1000 ms) on the blocking CDC read call so the
  task loop can also check for device disconnect without needing a second
  blocking primitive.

## 9. Lua execution model

### 9.1 Why Lua, and why coroutines

Lua 5.5, chosen over CircuitPython/MicroPython for this specific job:
smaller footprint, a crashed user script only aborts that one `lua_pcall`
rather than restarting the whole interpreter/kernel, and fast interpreter
startup if ever needed per-call. Coroutines let a parser be written as
straight-line "read until delimiter" code while actually running
incrementally across however many raw-data callbacks it takes to see a full
message — this replaces a hand-rolled C++ state machine per protocol.

**Why 5.5, not 5.4** (the version this document originally named, before 5.5
existed - 5.5.0 released 22 Dec 2025): checked the two series' actual
differences against what the sandboxing in §9.2 needs. `lua_sethook`/
`LUA_MASKCOUNT` (the instruction-count hook), `luaL_requiref` (restricted
library loading), `lua_resume`/`lua_yield` (coroutines), and `lua_register`
are all unchanged between 5.4 and 5.5 — verified against the 5.5 reference
manual's own "Incompatibilities with the Previous Version" chapter, not
assumed. The one C API change that touches us at all: `lua_newstate` gained
a third parameter (a hash seed, filled via the new `luaL_makeseed()`
helper) — a one-line adjustment to `lua_sandbox_create()`, not a design
concern. In exchange, 5.5 brings two concrete wins here: major garbage
collections now run incrementally (lower worst-case GC pause, relevant to
the tight USB-servicing timing budget in §8), and a new
`luaL_openselectedlibs()` helper that directly simplifies the
restricted-library setup §9.2 would otherwise hand-roll via repeated
`luaL_requiref` calls. Pulled in via the Espressif-maintained
`espressif/lua` component (ESP Component Registry) rather than vendored
source — lower integration risk, since Espressif has already worked out
the Xtensa/RISC-V build specifics, via the same `idf_component.yml`
pattern already used for `usb_host_bridge`'s `espressif/usb` dependency.

**Upstream packaging bug found while wiring this up**: `espressif/lua`
compiles the library itself with `LUA_32BITS=1` (so `lua_Number`/
`lua_Integer` are `float`/32-bit `int`), but as a `PRIVATE` compile
definition — it doesn't propagate to consumers. The component's own
`port/include/luaconf.h` "header injection", meant to fix exactly this,
never actually takes effect either: `lua.h`'s `#include "luaconf.h"`
resolves relative to `lua.h`'s own directory first, finding the real
`lua/luaconf.h` directly, before the compiler ever consults the
`port/include` search path. Net effect: any consumer that doesn't also
define `LUA_32BITS` sees `luaconf.h`'s non-32BITS defaults (`double`/
`int64_t`) while the actual compiled library uses `float`/`int32_t` —
every `lua_tonumber`/`lua_tointeger` call silently reads garbage. Fixed in
`components/lua_sandbox/CMakeLists.txt` with the same `target_compile_definitions(...
PRIVATE LUA_32BITS=1)` line. Reported upstream:
[espressif/idf-extra-components#873](https://github.com/espressif/idf-extra-components/issues/873).

**Implemented contract** (`lua_sandbox_load`/`lua_sandbox_resume`, in
`components/lua_sandbox/`): the uploaded script's text *is* the coroutine
body directly — not wrapped in `coroutine.create(function(first_chunk)
... end)` as sketched below. A top-level Lua chunk is already an implicit
vararg function, so `local buffer = ...` captures the first
`lua_sandbox_resume()` call's data directly; no `parser = coroutine.create(...)`
boilerplate for the script to get right, and no "which global holds the
coroutine" convention for the C side to assume. The examples below keep the
`coroutine.create`/named-function framing because that's how the original
design discussion shaped the two coroutine *shapes* — translate mentally to
"the chunk body is the function body".

**Revised from the original "two coroutine shapes" sketch, and from an
even earlier push/pull ("master"/"slave") framing in between**: **a
channel has no binding to any driver at all.** `channel_registry` knows
nothing about USB, I2C, or any other peripheral - it manages named,
independently scheduled coroutines, full stop. Which native functions (if
any) an uploaded script happens to call - `i2c_read()`, `usb_read()`,
none of them, several different ones in the same script - is entirely up
to that script; there is no "this channel is the I2C kind" concept
anywhere in the architecture to get in the way. That's what directly
answers the original worry that started this: reading several unrelated
devices from one script and combining them (e.g. a second sensor used
only to correct the first one's reading) is just two function calls
before one `set_metric()` - nothing about the channel itself needs to
know or care.

Each driver exposes its own named read function (`usb_read()`,
`i2c_read(addr, len)`, `dht22_read()`, ...), all sharing one contract:
call it, get bytes back (or `nil`). Whether a *particular* function
returns immediately or actually suspends the calling coroutine until a
driver task has an answer is that one function's own business - a
contract between that native function and Lua that the channel
architecture is entirely indifferent to. `local data = i2c_read(0x48, 2)`
and `local data = dht22_read()` read identically in a script even though
(today, as a plan - see below) the first would be an ordinary synchronous
C function and the second would suspend and resume via `lua_yieldk`.
Why they'd differ is a plain question of how long the underlying
operation takes, nothing more: a microsecond-scale I2C register
transfer is cheap enough to just do inline; a millisecond-scale,
timing-critical operation (DHT22's bit-banging) isn't, and has to
hand off to its own task instead of blocking the shared pump for that
long.

`i2c_read(addr, len)` needs an address because an I2C transaction always
targets one specific device on a shared bus - the address is only ever
known to whoever's issuing that particular request, so it has to be a
call-time argument. `usb_read()` needs no argument *today* simply because
there's only one USB device in scope right now; a future
`usb_read(device_id)` would only start mattering once there's more than
one to tell apart - not yet decided, since it isn't needed yet.

```lua
local buffer = ""
while true do
    local nl = buffer:find("\n")
    while not nl do
        buffer = buffer .. usb_read()
        nl = buffer:find("\n")
    end
    local line = buffer:sub(1, nl - 1)
    buffer = buffer:sub(nl + 1)
    local key, val = line:match("(%w+):([%d%.]+)")
    if key then set_metric(key, tonumber(val)) end
end
```

`usb_read()` is this design's name for what a script used to write as a
bare `coroutine.yield()` call directly. Internally it's still exactly
that same suspend (resumed once `channel_registry_push_data()` delivers
something for this channel, via the already-implemented/tested path
described below) - just moved inside a named native function instead of
written as raw `coroutine.yield()` syntax in the script's own source. A
script no longer needs to call bare `coroutine.yield()` itself for this -
that's `usb_read()`'s job now, not the script's, and has nothing
architecturally to do with USB specifically: any future driver whose read
needs to suspend-and-resume would use the exact same underlying
mechanism inside its own function.

**`coroutine.yield()`/`coroutine.yield(ms)` are the opposite of
driver-specific** - plain, generic cooperative-scheduling primitives, the
same thing any cooperatively-scheduled system has: hand control to other
channels for a bit, then come back.
- `coroutine.yield(ms)`: come back in about `ms` milliseconds, with no
  data (resumed with an empty argument). For a channel's own timing -
  e.g. deciding how often to re-check something, or breaking a long
  computation into chunks so the instruction-count hook (§9.2) never has
  a reason to fire and other channels still get a turn:
  `coroutine.yield(0)` (or any small value) comes back at the very next
  opportunity rather than being parked indefinitely - the right primitive
  for "yield control without really blocking," as opposed to a bare,
  argument-less `coroutine.yield()`.
- `coroutine.yield()` (bare, no value): suspends with no self-declared
  time budget at all - safe only for a driver function (like `usb_read()`
  above) that has its own other way of guaranteeing it'll eventually be
  resumed (there, a future push). A script calling it directly, with
  nothing else driving its resume, would simply never come back -
  there's no general-purpose "wait for something, anything, from
  whatever source" primitive, by design.

C++ scheduler side: each registered poll-coroutine carries a
`next_run_ticks`. The Lua pump task's main loop is a wait on "ring buffer
has data OR earliest next_run_ticks has elapsed", resuming whichever
coroutine is due. With the expected small number of channels (single
digits), a linear scan for the earliest due coroutine is fine; no need for
a priority queue unless the channel count grows far beyond what's in scope
here.

**Implemented** in `channel_registry` (not a separate "scheduler"
component - it already owns the channel map, so the per-channel
scheduling fields live there too): `channel_registry_resume()` itself
records `next_run_ticks` from the yielded delay and `last_resume_tick`,
both using the real clock; `channel_registry_pick_due_channel(now_ticks,
...)` does the linear "most overdue" scan described above, taking
`now_ticks` as a parameter (not reading the clock itself) so the selection
logic is unit-testable without real delays - same
write-real-time/read-with-injectable-now split as `metrics_store`'s
staleness logic (§6.3).

A freshly loaded channel is always due (gets its first "kick-start"
resume with no data, needed either way - every channel's script has to
start somehow, whatever it calls first, or nothing at all yet). After
that first resume, only two things make a channel due again: new pending
data (`channel_registry_push_data()`/`channel_registry_pick_channel_with_data()`),
or the channel itself explicitly yielding a number (a real self-reschedule).
A plain `coroutine.yield()` with no value deliberately does *not*
reschedule the channel - caught while implementing: without this, a
coroutine suspended mid-message (inside what's now `usb_read()`'s own
internal `coroutine.yield()`) would get spuriously re-resumed with no
data on the very next scheduler pass, which the buffering logic above
(no `or ""` fallback past the first line) would error on.

**Implemented** (DHT22/AM2302, the project's first real sensor): its
bit-banged timing (sub-100us pulses) has to run in a tight
interrupts-disabled C loop no Lua script's scheduling granularity could
meet, so the native driver (`dht22_source`) owns its own poll schedule
entirely independently - a dedicated FreeRTOS task bit-bangs the
single-wire protocol every ~2.5s (the sensor's own minimum sample
interval) and delivers the raw 5-byte reading via
`channel_registry_push_data()`, the same mechanism `usb_read()` relies on.
**Not yet cleaned up**: the uploaded script (`tools/scripts/dht22.lua`)
still calls bare `coroutine.yield()` directly instead of through a
wrapping `dht22_read()` function - it was written before this section's
naming convention existed, and should be fixed to match (wrap it like
`usb_read()`) next time it's touched, not kept as some sort of deliberate
exception - there isn't one anymore. The checksum/byte-
unpacking/sign-handling logic (the part worth testing) lives in Lua,
covered by Tier 1 (`test_dht22_script.c`) against canned byte strings. The
actual GPIO bit-banging is hardware-only and only verified on real
hardware (§12.4) - see `components/drivers/dht22_source/dht22_source.c`
for why: sub-100us-per-bit timing needs a short interrupts-disabled
critical section, which doesn't fit this document's unit-testable/
hardware-only split any more cleanly than that. (DHT22 itself isn't an
important driver - picked only to exercise the mechanism, and dropping it
from the project entirely is on the table - but it's a useful reminder
that not every read is cheap enough to do inline in the shared pump task;
anything slower or timing-critical needs its own task regardless of what
its Lua-facing function ends up looking like.)

`lua_pump` drives this: `lua_pump_step(now_ticks, instruction_limit)`
drains and resumes every channel with pending data, then resumes one due
poll channel, then checks for stalled channels - a plain function,
unit-testable the same way. `lua_pump_start()` wraps it in the actual
FreeRTOS task (Core 1, DESIGN.md §5), **simplified from "wait on data OR
deadline" to a fixed poll**: the task calls `lua_pump_step()` in a loop,
sleeping `LUA_PUMP_IDLE_SLEEP_MS` (20 ms) when a step found nothing to
do. A true event-driven wait (woken early by an incoming-data
notification) would need a task handle + notification wired through from
whatever produces the data (`usb_host_bridge`, not built yet) - deferred
as unneeded complexity for now: §8 already budgets "tens of milliseconds
of slack" before USB back-pressure matters, comfortably inside a 20 ms
polling granularity.

Each channel's coroutine is independent — separate local state (its own
`buffer`, its own `next_run_ticks`). **Revised from the original plan**:
rather than all channels sharing one `lua_State`, each channel gets its
own `lua_sandbox_t` (own root state, own memory cap, own single
coroutine) - this is what `components/lua_sandbox/` actually implements,
one instance per channel rather than one shared instance with multiple
coroutine slots. At the expected scale (single-digit channels), N times
the library-bootstrap cost (opening `base`/`table`/`string`/`math`/
`coroutine` per sandbox) is negligible against the 8 MB PSRAM budget, and
it buys the per-channel memory isolation the original plan explicitly
deferred as a "follow-up if needed" - here for free, not as a later
optimization. `channel_registry` holds the `channel name → lua_sandbox_t*`
mapping; `lua_sandbox_create()` is a plain, repeatable call with no shared
state between instances.

### 9.2 Sandboxing — required, since scripts are user-supplied over HTTP

Three independent protections, all required together (not alternatives):

**Memory cap**, via a custom allocator passed to `lua_newstate`:

```c
#define MAX_LUA_MEMORY (128 * 1024)

typedef struct {
    size_t current_used;
    size_t max_limit;
} lua_mem_control_t;

void* l_alloc_restricted(void *ud, void *ptr, size_t osize, size_t nsize) {
    lua_mem_control_t *mc = (lua_mem_control_t *)ud;
    if (nsize == 0) {
        if (ptr) { mc->current_used -= osize; free(ptr); }
        return NULL;
    }
    size_t new_total = mc->current_used + nsize - (ptr ? osize : 0);
    if (new_total > mc->max_limit) return NULL;
    void *new_ptr = realloc(ptr, nsize);
    if (new_ptr) mc->current_used = new_total;
    return new_ptr;
}
```

128 KB per `lua_State` is a starting point, not a measured figure — tune
once real scripts exist. With 8 MB PSRAM available, there's room to be
generous.

**CPU runaway protection**, via an instruction-count debug hook, active
only while `lua_resume` is actually running (so there's no steady-state
overhead):

```c
void instruction_hook(lua_State *L, lua_Debug *ar) {
    luaL_error(L, "Script execution timeout");
}

lua_sethook(L, instruction_hook, LUA_MASKCOUNT, 10000);
int status = lua_resume(L, NULL, nargs, &nresults);
lua_sethook(L, NULL, 0, 0);
```

10,000 instructions is comfortably above what a per-chunk line parser needs
(typically low hundreds), so the hook essentially never fires for correct
scripts, and the per-call overhead of counting (an increment-and-compare)
is negligible against string allocation and the C callback cost it's
wrapping. This only guards against runaway *CPU* use (e.g. a pathological
regex); it is not a wall-clock timeout — a script that yields correctly
but then never gets resumed again (e.g. device unplugged mid-message) is a
separate, logical timeout handled in the pump's scheduler loop (track time
since last successful resume per channel; past a threshold, discard and
recreate that channel's coroutine rather than leaving it parked forever).
**Implemented** as `channel_registry_recover_stalled_channels(now_ticks,
stall_ticks)`: recreates a channel's coroutine from the last successfully
loaded source (cached in the registry for exactly this) whenever
`now_ticks - last_resume_tick > stall_ticks`. A channel that's never had a
script loaded has nothing cached and is never considered stalled - there
would be nothing to recreate.

**Restricted API surface** — open `base`, `table`, `string`, `math`, and
`coroutine`; skip `os`, `io`, `debug`, `package` entirely (`package` is
what provides `require`, so excluding it removes that too); then strip
dangerous globals that `base` still exposes. `coroutine` is not in the
original discussion's list below, but has to be: a script's own code and
every driver read function alike (§9.1) rely on `coroutine.yield()` under
the hood, which doesn't exist without it — pure language-level control
flow, no OS/IO surface, safe to include. Lua 5.5's
`luaL_openselectedlibs()` (§9.1) replaces the
hand-rolled `luaL_requiref` loop this originally sketched:

```c
void setup_safe_lua_env(lua_State *L) {
    int load_mask = LUA_GLIBK | LUA_TABLIBK | LUA_STRLIBK | LUA_MATHLIBK | LUA_COLIBK;
    luaL_openselectedlibs(L, load_mask, 0);

    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");
    lua_pushnil(L); lua_setglobal(L, "print");
    lua_pushnil(L); lua_setglobal(L, "collectgarbage");

    lua_register(L, "set_metric", l_my_set_metric);
    lua_register(L, "i2c_read", l_my_i2c_read);
}
```

`collectgarbage` is removed so GC timing stays under firmware control
rather than being triggerable (or disable-able) by the script itself.

**Implemented**, `set_metric`/`i2c_read` split into four functions (one
per `metrics_store` metric kind) with `i2c_read` deferred to
`i2c_poll_source` work:

```lua
set_metric(name, value)               -- gauge
add_counter(name, amount)              -- counter; amount must be >= 0
init_histogram(name, {bounds...})     -- defines buckets, once before observing
observe_histogram(name, value)
```

Each is a C closure registered with the channel name as its *only*
upvalue (`push_metric_fns()` in `lua_sandbox.c`) - not a Lua global
variable, not a script argument. A script has no way to name a channel at
all, so it structurally cannot misattribute a metric to a different
channel, whether by accident or a deliberately hostile script. This is
why `lua_sandbox_create()` now takes the channel name as a parameter: the
sandbox didn't otherwise know which channel it belonged to.

A `metrics_store` failure (bad bucket count, name collision with a
different metric kind, `observe_histogram` before `init_histogram`, out
of metric slots) raises a Lua error via `luaL_error()` rather than
returning a status code the script could ignore - it surfaces through
the existing `lua_sandbox_resume()` error path exactly like any other
runtime error, with no new plumbing needed (DESIGN.md §6.3's
`lua_error` channel-status field).

`i2c_read` is intentionally not implemented yet - no I2C bus is brought
up anywhere in the firmware yet either, so there's nothing for it to
read from.

## 10. Script upload workflow

HTTP endpoint on the Core-0 server, e.g. `POST /scripts/<channel>` with the
raw Lua source as the body:

1. **Dry-run compile**: `luaL_loadstring` against the uploaded text (not
   against the live interpreter state) to catch syntax errors before
   touching anything live. On failure, respond `400` with Lua's own error
   message — no need to invent a separate error format.
2. **Persist**: write to LittleFS as `parser_<channel>_new.lua`.
3. **Atomic swap**: once written and validated, rename into place over the
   active `parser_<channel>.lua` and tear down/recreate just that channel's
   coroutine against the new script. The channel should never run with a
   half-written file — rename is the atomicity boundary, not a flag check
   mid-write.
4. The other channels' coroutines are unaffected by one channel's reload.

**Implemented**, following the literal order above: a new
`channel_registry_validate_script()` (dry-run compile + channel-exists
check, zero live effect - split out specifically for this) runs first; on
success `http_api` persists to LittleFS (write `_new`, then rename over
the active file); only once that succeeds does it call
`channel_registry_load_script()` to do the actual live swap. A flash
write failure is therefore a clean upload failure (500, logged as an
error) with no live effect at all - the channel keeps running whatever it
ran before, never something that's only in RAM and would vanish on
reboot. (Caught in review: an earlier version called the combined
validate+swap function first and persisted after, meaning a persist
failure left the channel running a script that looked fine - online,
metrics flowing - but wouldn't survive a reboot. Reordering so the live
swap never happens until durability is confirmed avoids needing any
rollback for that case.) Verified end-to-end on real hardware (upload,
syntax-error rejection, unknown-channel 404). **Not yet wired**: reloading
persisted scripts into channels at boot - there's nothing to reload into
until a driver (`usb_host_bridge`/`i2c_poll_source`) actually calls
`channel_registry_add_channel()`.

**Upload authentication (implemented, optional)**: given the LAN-only
trust assumption in §1, auth is off by default and `/scripts/*` behaves
exactly as described above with no auth at all. It's gated entirely by a
build-time public key, `CONFIG_UPLOAD_AUTH_ECDSA_PUBLIC_KEY` (Kconfig,
default empty) — set it and a new `upload_auth` component enforces a
Withings-style server-issued nonce plus an ECDSA-SHA256 (P-256) signature
on every upload. A server-issued nonce was chosen over a bare signed
timestamp specifically to avoid any dependency on §6.4's best-effort SNTP
— a device with no NTP reachable must still be able to accept signed
uploads, and §6.4's whole point is that nothing hard-depends on NTP
succeeding.

ECDSA (asymmetric) over HMAC (symmetric): the device only ever holds the
*public* key, not a shared secret. Anyone extracting firmware/flash from a
physical device recovers only the ability to verify signatures, not to
forge them — the private key lives wherever `tools/upload_script.sh` runs
and never has to touch the device at all. A shared HMAC secret can't offer
that: holding the device's copy of it is already enough to sign.

1. `GET /scripts/nonce` — always registered; if no key is configured it
   responds `404` with an explanatory message (no key means no nonce)
   rather than leaving the client to puzzle over the wildcard
   `/scripts/*` POST handler's generic `405` for a GET request. With a
   key configured, it returns a fresh single-use nonce (16 random bytes
   from `esp_fill_random()`, hex-encoded), held server-side in a small
   fixed-size table with a few minutes' validity tracked against the
   FreeRTOS tick count, not wall time. `/scripts/nonce` is necessarily
   unauthenticated (nothing to check a signature against yet), so the table
   is also a known, accepted DoS surface: anyone on the LAN issuing nonces
   faster than a legitimate uploader can evict its pending one first -
   not defended against, since uploads are rare manual actions at this
   project's scale.
2. The uploader signs SHA-256("<channel>,<nonce>,<body>") with its P-256
   private key — in practice, plain `openssl dgst -sha256 -sign
   key.pem` over the assembled message, no custom tooling — and sends the
   resulting DER-encoded signature as `X-Signature` (hex), the nonce as
   `X-Nonce`, alongside the usual `POST /scripts/<channel>` body. Binding
   the channel name into the signed string stops a nonce fetched for one
   channel being replayed against a different one.
3. The server converts the DER signature to PSA's fixed-width raw `r||s`
   form (`mbedtls_ecdsa_der_to_raw()` — DER is what every standard tool
   produces, PSA only accepts raw), then verifies it against the
   configured public key, and checks the nonce is known, unexpired, and
   unused — consuming it (single-use) on this check alone, regardless of
   what happens next, so the same signed request can never be replayed
   even if something later in the pipeline fails. A missing, malformed,
   expired, or already-used nonce/signature is rejected `401` before any
   script validation runs.

See `tools/upload_script.sh` for a reference curl+openssl client
implementing this flow (and the plain unauthenticated upload otherwise),
and `CONFIG_UPLOAD_AUTH_ECDSA_PUBLIC_KEY`'s Kconfig help for how to
generate a key pair and extract the public key's hex encoding.

`upload_auth` has no dependency on `esp_http_server` (only `mbedtls` for
ECDSA/SHA-256 and `esp_hw_support` for `esp_fill_random()`), so its nonce
table and signature logic are fully covered by Tier 1 host tests
(`test_upload_auth.c`, built against a test key pair pinned in
`test/host/sdkconfig.defaults`/the test file itself) — `http_api`'s own
header plumbing/wiring is exercised on real hardware only, same carve-out
as the rest of §12.2's `http_api` logic.

LittleFS over SPIFFS: chosen for power-loss resilience (atomic writes)
and better performance; reserve a partition in `partitions.csv`:

```
# Name,   Type, SubType, Offset,  Size, Flags
storage,  data, spiffs,        ,  2M,
```

(the `spiffs` subtype name is what ESP-IDF's partition tool expects even
though the filesystem mounted there is LittleFS).

## 11. HTTP server summary

| Endpoint | Method | Behavior |
|---|---|---|
| `/metrics` | GET | Reads the current snapshot only (§6.2); never blocks on I/O or Lua. |
| `/scripts/<channel>` | POST | Validates, stores, atomically swaps a channel's Lua script (§10). If `CONFIG_UPLOAD_AUTH_ECDSA_PUBLIC_KEY` is set, requires `X-Nonce`/`X-Signature` headers (§10). |
| `/scripts/nonce` | GET | Issues a single-use nonce for the next upload (§10). Always registered; `404` with an explanatory message if `CONFIG_UPLOAD_AUTH_ECDSA_PUBLIC_KEY` isn't set. |
| `/` | GET | Operator's manual, rendered to HTML (§11.1). |
| `/manual.md` | GET | The same manual, raw Markdown (§11.1). |
| `/openapi.yaml` | GET | OpenAPI 3.x spec for this table (§11.1). |

### 11.1 Self-documentation endpoint for humans and agents

**Implemented** (`components/self_doc/`, `components/uzlib_inflate/`,
`docs/manual.md`, `docs/openapi.yaml`). The goal: once hardware is debugged
and Lua callbacks (§9) are established, nothing beyond the device itself
should be needed to operate it — the manual lives on the device, so it's
always in sync with the firmware actually running.

Three flat routes, deliberately *not* under `.well-known/` (see rejected
standards below):

| Endpoint | Behavior |
|---|---|
| `GET /` | Pre-rendered HTML version of the manual, for a human opening the device's IP in a browser. |
| `GET /manual.md` | The same manual, raw Markdown — for agents/tools that would rather parse plain text than strip HTML. This is the stable URL other machine-readable docs link to. |
| `GET /openapi.yaml` | OpenAPI 3.x spec for `/metrics`, `/scripts/*`, etc., with `info.externalDocs.url` pointing at `/manual.md` (`externalDocs` is a real OpenAPI field for linking to supplementary documentation) — the discovery path for an agent that already knows to look for OpenAPI. |

`docs/manual.md` is the manual's only source of truth — it is *not*
DESIGN.md (this document): DESIGN.md records engineering decisions and
rationale for whoever implements or maintains the firmware; `docs/manual.md`
is the end-user/agent-facing operator's manual, the thing actually served
at `/manual.md` and (rendered) at `/`.

**Rejected**: a `.well-known/ai-discovery.json`-style convention, surfaced by
an AI web search that turned out to be unreliable on checking. The cited
"IETF draft" (`draft-aiendpoint-ai-discovery`) is a real document but an
unadopted *individual* submission with no working-group standing (checked
directly against the IETF datatracker — it even proposes a different path,
`/.well-known/ai`, than what the search summary claimed). A second cited
"standard" (`agent-manifest-spec.org`) solves an unrelated problem — agents
publishing their own identity/risk profile, not a device publishing its API.
`.well-known/` itself (RFC 8615) is real, but no established convention
calls for any of these three files to live there, so doing so would be pure
cosmetics. Revisit only if a genuinely adopted standard appears later.

**Discovery for an agent that lands on `/` directly**, without ever finding
`/openapi.yaml`: the rendered HTML carries both a
`<link rel="alternate" type="text/markdown" href="/manual.md">` in `<head>`
(the standard HTML mechanism for pointing at an alternate representation,
same pattern as an RSS `<link>`) *and* a plain visible link in the body —
because many simple HTML-to-text extraction pipelines (Readability-style
content extraction, as used by some agent harnesses) drop `<head>` entirely
and only look at visible body text.

**Build-time, not runtime** is the principle behind every decision below:
do the work once when building firmware; the device only ever serves static
bytes.

- **Compression, always decompressed before serving**: the manual and its
  generated HTML form are stored on flash as raw DEFLATE (RFC 1951, max
  level, *no* gzip/zlib container — nothing on either side ever needs to
  parse a header) purely to save flash — then **always** decompressed
  on-device before being sent, to every client, regardless of what (if
  anything) it sends as `Accept-Encoding`. Deliberately simplified away
  from content negotiation (checking `Accept-Encoding: gzip`, passing the
  blob through raw for capable clients, erroring for others): a small
  decompression library is cheap enough that running it on every request
  removes all of that branching/error-case logic for a one-off, negligible
  CPU cost per request.
  - **`uzlib`, not `puff.c`** (the two candidates from earlier discussion):
    `puff.c` only exposes a single whole-buffer-in, whole-buffer-out call,
    with no way to resume mid-stream — incompatible with the "decompresses
    straight into the HTTP response as output is produced" requirement
    below. `uzlib`'s `uzlib_uncompress()` is an explicit incremental state
    machine (bounded output buffer per call, resumable), which is what
    `components/uzlib_inflate/` wraps. Measured compiled size (esp32s3,
    `idf.py size-components`): 1.8 KB flash (1.6 KB code, under 0.2 KB
    data) for just the raw-inflate path (no gzip/zlib header parsing, no
    checksum, no compressor side) — at the low end of the "a few KB"
    estimate this section originally worked from.
  - A build-time constant, one `original_size` per asset
    (`self_doc_sizes.h`, generated by `components/self_doc/gen_assets.py`),
    is generated alongside the compressed blobs — known from the source
    file at build time, so nothing ever needs decompressing just to learn
    it. The compressed size needs no separate bookkeeping: it's already
    available for free from the embedded blob's own linker symbols
    (`_binary_..._start`/`_end`, as produced by ESP-IDF's `EMBED_FILES`).
  - That constant is what lets the response be both streamed *and*
    declare `Content-Length` up front: the handler builds the header block
    by hand (`esp_http_server`'s `httpd_resp_send_chunk()` always emits
    `Transfer-Encoding: chunked` with no way to opt out, so this couldn't
    use it) with `Content-Length: original_size`, writes it via the
    low-level `httpd_send()`, then feeds `uzlib_uncompress()`'s output
    straight to the same socket in small (1 KB) chunks via a 32 KB sliding
    LZ77 window kept separately from those chunks — never buffering the
    whole decompressed document in RAM. No double decompression (one pass
    to measure, one to send), and no chunked transfer-encoding.
  - Why DEFLATE and not zstd/brotli: the smallest usable decompressors
    exist only for DEFLATE (`puff.c`, `uzlib` — a few KB each, confirmed
    above); zstd/brotli decoders are themselves tens of KB because of
    their more complex formats (FSE/Huffman tables, context dictionaries),
    which would defeat the point of a "minimal" decoder.
- **HTML rendering**: the manual's HTML form is also generated at build
  time, via the Python `markdown` library (`extensions=["tables",
  "fenced_code"]` — both used by `docs/manual.md`) wrapped in a small
  hand-written standalone-page template in `gen_assets.py`, not with an
  on-device Markdown library. Originally built against `pandoc` (a host
  binary, found via `find_program` and presumed pre-installed); switched
  to `markdown` for two reasons: it's a Python package, and `idf.py`
  already requires - and manages its own venv for - Python, so this
  avoids a second, separate "install this host tool yourself" step; and
  it rendered noticeably smaller HTML than pandoc's default standalone
  template once pandoc's bundled CSS was dropped (21.8 KB → 9.0 KB
  uncompressed, measured, for the same source document). `cmark` was
  also considered, for the same reason as `pandoc` - a host binary, not
  a Python package - and rejected for the same reason.
  - **No separate "please install X" step**: `markdown` is a build-time-
    only dependency (rendering happens once per build, on the host;
    nothing it does or produces beyond the rendered HTML text ever
    reaches the device) pinned to an exact version and installed
    automatically into ESP-IDF's own managed Python environment by
    `components/self_doc/CMakeLists.txt` the first time it's missing
    (checked via `idf_build_get_property(... PYTHON)` - the same
    interpreter `idf.py` itself uses, not a fresh `find_package(Python3)`
    search that could resolve to an unrelated system Python) - the same
    "first run sets itself up" experience `idf.py`/`export.sh` already
    give the rest of the toolchain, rather than a separate manual
    prerequisite nobody would think to document. Confirmed working from
    a clean ESP-IDF Python environment (no network access at all beyond
    that one-time install would make this a build error, with a message
    naming the exact manual install command as a fallback).
  - The `<link rel="alternate" type="text/markdown" href="/manual.md">`
    (head) and the visible "Raw Markdown version" link (body) from the
    discovery paragraph above are injected by `gen_assets.py` directly
    into the rendered HTML fragment's wrapping template - no separate
    flags/temp files to keep in sync with a host tool's own version.
  - An on-device renderer was considered and rejected:
  `md4c` is the smallest CommonMark-compliant option but still tens of KB
  compiled; a hand-rolled subset parser could be a few KB, but buys
  nothing over build-time generation, which costs zero flash and zero
  runtime. A client-side JS renderer (`marked.js`, the lightest common
  option — measured at ~40 KB minified / ~12 KB gzipped, not assumed) was
  rejected even harder: it requires a JS engine on the client, defeating the
  point of this endpoint for agents that just do a plain HTTP GET.
  - `openapi.yaml` is embedded uncompressed (`EMBED_FILES`, not run
    through the pipeline above) - at 5 KB raw it's small enough that
    build-time compression wasn't worth the added complexity; combined
    on-flash cost of all three assets plus `uzlib_inflate` is ~17 KB
    (measured: `libself_doc.a` 14.8 KB + `libuzlib_inflate.a` 1.8 KB),
    against a 16 MB flash budget.

## 12. Testing strategy

No Linux-hosted emulator models ESP32-S3 USB Host faithfully (Espressif's
own QEMU fork explicitly lists USB, SPI and I2C as unsupported; community
Renode ports are partial and don't model USB Host either; Wokwi only does
USB-CDC *device* mode). Rather than fight that, the architecture pushes
everything that *can* be tested off-hardware into its own layer, and keeps
the genuinely hardware-only surface as small and thin as possible.

### 12.1 The `channel_source` interface

This is the one abstraction the whole strategy hinges on. Every data
channel (USB CDC today, I2C polling later) is consumed by the scheduler
and Lua pump (§9) only through a small interface, never through
driver-specific calls scattered in the logic:

```c
typedef struct {
    esp_err_t (*open)(void *ctx);
    // Blocking read with timeout; returns bytes read, 0 on timeout, <0 on error/disconnect.
    int (*read)(void *ctx, uint8_t *buf, size_t maxlen, uint32_t timeout_ms);
    bool (*is_connected)(void *ctx);
} channel_source_t;
```

- **Real implementations** (`usb_host_bridge`, `i2c_poll_source`): thin —
  they do nothing but move bytes in and out and report connection state.
  No parsing, no scheduling, no Lua calls live here. This is deliberate:
  the less logic lives behind the interface that can't be tested off
  hardware, the less needs validating on real hardware at all.
- **Fake implementation** (`channel_source_fake`, in `channel_source/`):
  reads from a file, a Unix socket, or an in-memory queue fed by a test.
  Selected at build time via a Kconfig option (exact option name TBD at
  implementation time, e.g. `CONFIG_CHANNEL_SOURCE_FAKE`), not a runtime
  branch — keeps the real driver code fully compiled out of host/QEMU
  test builds instead of merely unreached.

### 12.2 Tier 1 — host-native unit tests (fast, no emulator at all)

Everything that doesn't need the interface above at all, or only needs the
fake implementation, builds and runs as a plain POSIX binary via ESP-IDF's
Linux target (`idf.py --preview set-target linux`) using Unity:

- Lua sandbox: allocator memory-cap behavior, instruction-hook firing,
  stripped-API enforcement, coroutine yield/resume semantics for both a
  data-driven resume (the mechanism behind `usb_read()`) and a
  self-rescheduling `coroutine.yield(ms)` (§9).
- Scheduler: "earliest due coroutine" selection logic, logical-timeout
  detection for a stalled mid-message coroutine (§9.1).
- `metrics_store`: staleness-threshold omission logic, the always-present
  health metric, Prometheus text-format output.
- `channel_registry`: channel add/remove, the script-upload decision flow
  (dry-run compile against a throwaway state, then atomic swap only on
  success, never disturbing a channel's existing script on a rejected
  upload — DESIGN.md §10's actual implementation), and the two scheduler
  decision functions from §9.1/§9.2: "earliest due coroutine" selection
  and stalled-channel recovery, both exercised with an injected
  `now_ticks` rather than real delays.
- `lua_pump_step()`: the actual pass-draining-data/resuming-due-channel/
  recovering-stalled-channels orchestration, as a plain function
  independent of the real FreeRTOS task. `lua_pump_start()`/`_stop()`
  (the real task, Core 1 pinned) get one lifecycle-only test on the Linux
  target too - it does support real tasks - but only checks start/run/stop
  doesn't crash or leak, not the task's real-time behavior.
- `http_api_handle_script_upload` is a one-line passthrough to
  `channel_registry_load_script()`, so its logic is covered by
  `channel_registry`'s tests, not a dedicated test of its own —
  `http_api` itself isn't in the Tier 1 build at all, since pulling in
  its `PRIV_REQUIRES upload_auth`/`channel_registry`/`littlefs` wiring
  for a one-liner isn't worth it.
- `self_doc` (§11.1) **is** Tier 1 tested, despite needing
  `esp_http_server` (and transitively `lwip`) - contrary to this
  document's own earlier assumption that those were "unproven on the
  Linux target": a real `httpd_server` does start and serve real TCP
  sockets there (confirmed first with a manual smoke test - an ad hoc
  scratch project, curl against a running instance - before writing an
  automated one). `test_self_doc_http.c` starts one real server once and
  does real `GET` requests against all three routes (and one unknown
  path, for the always-registered 404) over a real client socket in the
  same process, asserting on status/Content-Type/Content-Length/body
  contract - not a byte-for-byte diff against `docs/manual.md`, which
  would make every prose edit break this test for no reason.
  - **Found while writing it**: on this target, `connect()`/`send()`/
    `recv()` can all return `-1`/`EINTR` well before the peer actually
    closed or failed (FreeRTOS's Linux port ticks via a periodic signal,
    which interrupts blocking syscalls) - a plain retry-the-syscall loop
    on `EINTR` still hung forever, because busy-retrying a blocking
    syscall without ever yielding starves `httpd`'s own worker task of
    a chance to run at all. `vTaskDelay(1)` between retries fixes it -
    `test_self_doc_http.c`'s `RETRY_ON_EINTR` macro.
  - `uzlib_inflate` (hardware-independent, no sockets needed) is Tier 1
    tested too: a known raw-DEFLATE vector round-tripped across its
    internal chunk-buffer boundary, plus sink-error and truncated-input
    handling.

This tier is the bulk of the test suite by design — it is where almost all
of the actually interesting logic in this project lives (§9, §6, §10), and
it runs in milliseconds per test with no flashing and no emulator startup
cost, so it's what should run on every change.

### 12.3 Tier 2 — QEMU integration tests (full firmware, fake channel data)

A real firmware image, built for the `esp32s3` QEMU machine, boots under
`idf.py qemu` with the real FreeRTOS task layout and core pinning from §5,
`channel_source_fake` standing in for USB/I2C, and networking over QEMU's
emulated OpenCores Ethernet model (not the real W5500 driver — `net_eth`'s
W5500-specific bring-up is swapped out for QEMU's netif in this build
variant). This validates things Tier 1 can't:

- The HTTP server actually serving `/metrics` and `/scripts/*` over a real
  socket, end-to-end through the Lua pipeline, under real task priorities
  and the Task Watchdog.
- Startup/boot sequencing, including the maintenance-mode GPIO read (§4)
  with the pin level driven by the test rather than a real jumper.
- Multi-channel scheduling under the real scheduler loop rather than
  calling scheduler functions directly as in Tier 1.

Test driver scripts live in `test/qemu/`: start `idf.py qemu`, feed bytes
into the fake channel's backing file/socket, and curl the exposed HTTP
ports to assert on `/metrics` output.

**Deliberately not pursued yet** (`test/qemu/run_qemu_test.sh` is still a
stub that exits 1): the actual cost turned out larger than it first
looked, for a test that's really only covering `http_api`'s httpd
plumbing plus task-priority/boot-sequencing concerns - everything else
this would exercise is already covered by Tier 1 against the real
functions directly. Would need, in order: (1) a `channel_source` Kconfig
switch (name still TBD, §13) plus a small glue task to actually pump
`channel_source_fake` into `channel_registry` - nothing currently calls
`channel_source_fake`'s `open`/`read`/`is_connected` from anywhere; (2)
`net_eth`'s QEMU-netif variant (the OpenCores MAC, not W5500 - a second
code path through `net_eth_init()`); (3) the QEMU build variant and
networking setup itself. Revisit if `http_api` grows enough
request-handling logic that manual curl-against-real-hardware checks
(current practice, §12.4) stop being enough, or if boot-sequencing bugs
start actually occurring that Tier 1 can't catch.

### 12.4 Tier 3 — hardware smoke tests (the board you already have)

Reserved for exactly the things Tiers 1–2 structurally cannot cover:

- `usb_host_bridge` against a real USB CDC device — enumeration, NAK
  behavior under a deliberately slow consumer, disconnect handling.
- ~~`net_eth`'s real W5500 SPI bring-up~~ — done: link-up, DHCP lease, and
  SNTP sync all confirmed on real hardware against the LAN, board reachable
  by ping. Deferred: a static-IP fallback if DHCP is ever unavailable, and
  verifying reconnect behavior after a cable pull.
- `i2c_poll_source` against real I2C sensors.
- AM2302/DHT22 against real hardware (`dht22_source`'s bit-banged read).
- The GPIO boot-mode strap (§4) with an actual jumper/button.
- PoE power budget / 5 V-rail ADC reading (§7) under real USB client load.

Because the thin-interface design keeps these drivers free of business
logic, a hardware smoke test for each is a short, mostly manual checklist
rather than a full test suite — the logic it would otherwise need to
re-verify is already covered by Tier 1.

## 13. Explicitly open items for the implementing agent

These were raised in discussion but not resolved — don't silently pick an
answer, surface them:

- Exact GPIO pin and polarity for the USB device/host boot-mode switch
  (§4) — depends on which pins the Waveshare board leaves free of
  boot-strapping requirements.
- Staleness threshold value and whether it's fixed or derived per channel
  (§6.3).
- Whether multiple channels sharing one `lua_State` (as currently
  recommended, §9.1) is acceptable once there's a real script misbehaving,
  or whether per-channel isolation is needed from day one.
- Whether to keep `dht22_source` at all (it's there only to exercise the
  mechanism) - and if so, wrapping its native read in a `dht22_read()`
  function (via `lua_yieldk`) and updating `tools/scripts/dht22.lua` to
  call it instead of a bare `coroutine.yield()`, for consistency with
  `usb_read()`/`i2c_read()` (§9.1).
- How to address more than one USB device from Lua, if that's ever
  needed (`usb_read(device_id)` or similar - deferred, since only one
  device is in scope right now, §9.1).
- Whether `i2c_read(addr, len)` is implemented as a plain synchronous C
  function (likely fine - a register transfer is microseconds against
  the scheduling budget) or also needs `lua_yieldk`, once there's a real
  I2C driver and bus to measure against instead of assuming (§9.1).
- ~~Upload endpoint authentication, if the deployment target isn't a fully
  trusted LAN~~ — resolved: a server-issued nonce (`GET /scripts/nonce`)
  plus an ECDSA-SHA256 (P-256) signature over `"<channel>,<nonce>,<body>"`,
  verified against the optional `CONFIG_UPLOAD_AUTH_ECDSA_PUBLIC_KEY`
  build-time public key, implemented in the new `upload_auth` component
  (§10).
- ~~Confirm from the actual Waveshare schematic which SPI bus W5500 is
  on~~ — resolved: pins confirmed from the official Waveshare wiki and
  verified against real hardware (§2). Still open: whether the *other*
  SPI bus (SPI3_HOST, since W5500 defaults to SPI2_HOST) is actually free
  for sensors on this board, or shares pins with something else — not
  yet checked.
- Exact Kconfig option name and build-variant wiring for selecting
  `channel_source_fake` vs. the real drivers (§12.1), and how `net_eth`
  swaps between the W5500 driver and QEMU's netif for Tier 2 builds.
- Whether histogram bucket storage (§6.5) should be steered to PSRAM via
  `heap_caps_malloc`/`MALLOC_CAP_SPIRAM` instead of plain `malloc()` —
  revisit if a script ever defines many large histograms at once.
- ~~Which minimal inflate library (`puff.c` vs. `uzlib`, §11.1) and which
  host-side tool (`cmark`, `pandoc`, or something else) generates the
  build-time HTML from the Markdown manual~~ — resolved: `uzlib`, because
  its incremental `uzlib_uncompress()` can resume mid-stream and `puff.c`'s
  single whole-buffer call can't; the Python `markdown` package (not
  `cmark`/`pandoc`, both standalone host binaries), because `idf.py`
  already requires and manages its own Python environment, so a pip
  package can install itself into it automatically with no separate
  manual step, while a host binary cannot (§11.1).
- Summary-style metrics (client-side quantiles) were deliberately skipped:
  Prometheus's own Summary type exposes only the final quantile values, not
  the underlying estimator state, so it can't be aggregated server-side
  across instances the way a histogram can via `histogram_quantile()`.
  Note for whoever revisits this: a mergeable sketch (e.g. t-digest)
  *would* support cross-instance aggregation if its centroids were exposed
  and merged outside Prometheus's own model — that's a different, bigger
  feature than "add a Summary type" and should be scoped separately if it
  ever becomes a real requirement.

## 14. Language choice: why C++, not Rust/Nim/Zig

Decision: modern C++ with ESP-IDF. Rust, Nim, and Zig were each checked
first, since avoiding classic C/C++ UB was an explicit goal:

| | USB Host (central feature) | Ethernet/W5500 | Lua embedding | Toolchain risk |
|---|---|---|---|---|
| **Rust** | no_std: no host-mode driver exists at all. std: no bindings yet, feasible via FFI (`esp-idf-sys` `extra_components`) | solid — `esp-idf-svc` reuses the same `esp_eth` driver | `mlua`: open, unresolved linker bug on Xtensa | fork of rustc/LLVM for Xtensa |
| **Zig** | same gap as Rust, with no ecosystem around it | unproven | unproven | also forked/patched LLVM, "very early" per Zig's own 2026 notes |
| **Nim** | no existing binding (manual FFI, like Rust) | no existing binding (manual FFI) | should work — plain C FFI, no special build step | none — compiles to C, reuses the existing `xtensa-esp32s3-elf-gcc` |

Rust's gap (no USB Host track at all in `esp-hal`, plus an unresolved
`mlua`/Xtensa linker bug — [mlua-rs/mlua#642](https://github.com/mlua-rs/mlua/issues/642))
rules it out for now; worth revisiting only if that issue closes. Zig
carries Rust's same Xtensa-toolchain risk with none of its ecosystem. Nim
avoids the toolchain risk entirely but its ESP-IDF bindings (`Nesper`) are
a single rough hobby project with no prior art for USB Host or W5500 — the
FFI work would match Rust's, on a much less battle-tested base. Risk/reward
favors plain C++ across the board.

**Mitigating the actual underlying worry (UB) without changing language**:
build the Tier 1 host-native unit tests (§12.2) with
`-fsanitize=address,undefined` on the Linux target — catches buffer
overruns, use-after-free, signed overflow, and misaligned access on every
test run, at zero cost to the embedded build. Wired in (`test/host/CMakeLists.txt`)
once `metrics_store` got a real implementation with actual heap allocation
(§6.5's histogram buckets) worth catching bugs in; carry the same flags
forward once `lua_sandbox`/the scheduler get real implementations too.
