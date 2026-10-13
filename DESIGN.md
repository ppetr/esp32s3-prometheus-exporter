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
    usb_host_bridge/      # real impl: USB Host CDC-ACM task, ring buffer handoff
    i2c_poll_source/      # real impl: I2C register polling
    lua_sandbox/          # lua_State setup, allocator, hooks, safe API — hardware-independent
    channel_registry/     # channel name -> lua_sandbox_t* map + scheduling (§9.1-§9.2) — hardware-independent
    lua_pump/              # the Lua pump task itself (§5, §9.1) — step logic hardware-independent, task creation Tier 2/3
    metrics_store/        # metric map + staleness logic — hardware-independent
    http_api/              # /metrics and /scripts endpoints — hardware-independent except for the httpd dependency
    net_eth/               # W5500 init, SNTP — real hardware only
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
and is the most likely early warning for USB-inrush-induced brownouts.

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

Two coroutine shapes, both driven from the Lua pump task on Core 1:

**A. Stream-driven parser** (USB CDC data arrives asynchronously):

```lua
parser = coroutine.create(function(first_chunk)
    local buffer = first_chunk or ""
    while true do
        local nl = buffer:find("\n")
        while not nl do
            local chunk = coroutine.yield()
            buffer = buffer .. chunk
            nl = buffer:find("\n")
        end
        local line = buffer:sub(1, nl - 1)
        buffer = buffer:sub(nl + 1)
        local key, val = line:match("(%w+):([%d%.]+)")
        if key then set_metric(key, tonumber(val)) end
    end
end)
```

C++ side resumes it every time new USB bytes arrive for that channel:

```c
lua_pushlstring(L, data, len);
int status = lua_resume(L, NULL, 1, &nresults);
```

**B. Poll-driven parser** (I2C or any source the device must be actively
queried, on a schedule rather than reacting to pushed data):

```lua
function i2c_poll_loop()
    while true do
        local data = i2c_read(0x48, 2)
        if data then
            local temp = (data:byte(1) * 256 + data:byte(2)) / 16.0
            set_metric("room_temp", temp)
        end
        coroutine.yield(5000)  -- ms until next run
    end
end
```

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
resume with no data, needed either way - an I2C-poll script's first run
has to start somehow, and a stream parser's own first line defaults
`first_chunk or ""`). After that first resume, only two things make a
channel due again: new pending data
(`channel_registry_push_data()`/`channel_registry_pick_channel_with_data()`),
or the channel itself explicitly yielding a number (a real poll
schedule). A plain `coroutine.yield()` with no value deliberately does
*not* reschedule the channel - caught while implementing: without this,
a stream parser paused mid-message (`local chunk = coroutine.yield()`,
waiting for the next chunk) would get spuriously re-resumed with no data
on the very next scheduler pass, which a script following §9.1-A's own
example (no `or ""` fallback past the first line) would error on.

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
original discussion's list below, but has to be: both coroutine shapes in
§9.1 call `coroutine.yield()` from inside the script, which doesn't exist
without it — pure language-level control flow, no OS/IO surface, safe to
include. Lua 5.5's `luaL_openselectedlibs()` (§9.1) replaces the
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

**Implemented**, with one deliberate reordering from the steps above:
`channel_registry_load_script()` already does dry-run-then-live-swap as
one atomic in-memory operation (§9.1's existing, separately-tested logic),
so `http_api`'s handler calls that first and only persists to LittleFS
(write `_new`, then rename over the active file) on success - an
unvalidated script is thus never written to flash at all. Persistence is
best-effort: a flash write failure after a successful live swap is logged
and still returns 200, since the upload's live effect genuinely succeeded;
only durability across a reboot is lost. Verified end-to-end on real
hardware (upload, syntax-error rejection, unknown-channel 404). **Not yet
wired**: reloading persisted scripts into channels at boot - there's
nothing to reload into until a driver (`usb_host_bridge`/
`i2c_poll_source`) actually calls `channel_registry_add_channel()`.

**Not yet decided / explicitly flagged as open**: the conversation never
addressed upload authentication. Given the LAN-only trust assumption in
§1, this is fine for a single-tenant deployment on a trusted network, but
if that assumption changes, `/scripts/*` needs some form of auth before
being exposed beyond a lab network — raise this with whoever owns the
deployment environment rather than assuming either way.

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
| `/scripts/<channel>` | POST | Validates, stores, atomically swaps a channel's Lua script (§10). |

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
  stripped-API enforcement, coroutine yield/resume semantics for both the
  stream-parser and poll-parser shapes (§9).
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
  `http_api` itself isn't in the Tier 1 build at all, since it
  `PRIV_REQUIRES esp_http_server` (and transitively `lwip`), unproven on
  the Linux target and not worth pulling in for a one-liner.

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

### 12.4 Tier 3 — hardware smoke tests (the board you already have)

Reserved for exactly the things Tiers 1–2 structurally cannot cover:

- `usb_host_bridge` against a real USB CDC device — enumeration, NAK
  behavior under a deliberately slow consumer, disconnect handling.
- ~~`net_eth`'s real W5500 SPI bring-up~~ — done: link-up, DHCP lease, and
  SNTP sync all confirmed on real hardware against the LAN, board reachable
  by ping. Deferred: a static-IP fallback if DHCP is ever unavailable, and
  verifying reconnect behavior after a cable pull.
- `i2c_poll_source` against real I2C sensors.
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
- Upload endpoint authentication, if the deployment target isn't a fully
  trusted LAN (§10). If/when needed, a shared-secret HMAC over the request
  body (or a plain bearer token) is a better fit for this project's scale
  than full RFC 9421 HTTP Message Signatures - that spec's canonicalization
  and multi-algorithm negotiation solve interop between mutually
  untrusting parties, which doesn't apply to a single trusted uploader.
  Not implemented; `scripts_post_handler()` in `http_api.c` would need one
  early check added.
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
