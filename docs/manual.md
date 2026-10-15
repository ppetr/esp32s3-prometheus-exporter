# ESP32-S3 Telemetry Bridge — Operator's Manual

This is the device's own manual: it is served directly by the firmware at
`GET /` (rendered to HTML) and `GET /manual.md` (this file, raw), so it is
always in sync with whatever firmware is actually running — no external
documentation needed. See `/openapi.yaml` for a machine-readable endpoint
list; this document covers everything that spec can't express, in particular
how to write and upload a channel's Lua parser script.

If you are an agent: this file is the primary thing to read before doing
anything else with this device. The three self-documentation routes are
flat, not under `/.well-known/`.

## 1. What this device does

It bridges one or more peripheral devices (USB-serial, I2C, or a
directly-wired sensor) into Prometheus metrics. Each peripheral is a named
**channel**. Making a channel report useful numbers means uploading a small
Lua script that turns its raw bytes into metric values — there is no other
way to add a channel's metrics; the device has no built-in protocol parsers.

## 2. Reading metrics: `GET /metrics`

Plain-text Prometheus exposition format, e.g.:

```
room_temp{channel="i2c0"} 21.4
humidity_percent{channel="dht22"} 47.0
usb_bridge_channel_info{channel="dht22",status="ok"} 1
```

Notes:

- Every metric carries a `channel="<name>"` label — there is no global
  (channel-less) metric.
- A metric that hasn't been set recently (stale) is simply omitted from the
  response for that scrape, rather than reported as a stale value.
- `<channel>_info{channel="...",status="ok"|"error"[,lua_error="..."]} 1` is
  emitted automatically per channel — `status="error"` plus the `lua_error`
  label means the channel's script raised a Lua error and stopped running;
  check it before concluding a channel is simply quiet.
- A histogram metric named `foo` expands to `foo_bucket{...,le="<bound>"}`
  (one per bucket, plus a `le="+Inf"` bucket), `foo_sum`, and `foo_count` —
  standard Prometheus histogram shape, directly compatible with
  `histogram_quantile()`.
- No `# HELP`/`# TYPE` comment lines are emitted.

This endpoint never blocks on USB/I2C/Lua; it reads the last values each
channel's script reported.

## 3. Writing a channel's Lua script

A script is a Lua 5.5 chunk. Four functions are hardware-independent and
always available, regardless of channel:

| Function | Effect |
|---|---|
| `set_metric(name, value)` | Sets a gauge. Last call wins; overwrites the previous value for `name`. |
| `add_counter(name, amount)` | Adds a non-negative `amount` to a counter. Errors on a negative amount. |
| `init_histogram(name, {b1, b2, ...})` | Declares/resets a histogram's bucket upper bounds (strictly ascending). Call once before observing. |
| `observe_histogram(name, value)` | Records one observation into a histogram already declared with `init_histogram`. |

All four implicitly operate on the channel the script was uploaded to —
there is no channel argument.

### 3.1 Lua language surface

Beyond the four functions above (and the per-driver read functions in
§3.2), a script gets the following standard library tables and nothing
else - no `os`, `io`, `debug`, or `require` (and nothing it could load
one with: `package` isn't loaded either):

| Available | Not available |
|---|---|
| Base/global functions (`pairs`, `ipairs`, `type`, `tostring`, `tonumber`, `pcall`, `xpcall`, `error`, `assert`, `select`, `setmetatable`, `rawget`/`rawset`, ...) | `print`, `dofile`, `loadfile`, `collectgarbage` - removed individually even though the base library itself is loaded |
| `table.*` (`insert`, `remove`, `concat`, `sort`, ...) | `os.*`, `io.*`, `debug.*` |
| `string.*` (`find`, `match`, `gmatch`, `sub`, `byte`, `format`, `gsub`, `pack`/`unpack`, ...) - also reachable as `buffer:find(...)` method-call style | `require`/`package.*` |
| `math.*` | |
| `coroutine.*` (`yield`, `wrap`, `status`, ...) | |

`collectgarbage` is removed so GC timing stays under the firmware's
control, not the script's.

### 3.2 Reading from a driver

**A channel has no fixed binding to any driver.** There is no "this
channel is the I2C kind" setting anywhere - which native functions (if
any) your script calls is entirely up to the script. Nothing stops one
channel from reading two unrelated devices and combining them before
reporting a metric (e.g. a second sensor used only to correct the
first one's reading):

```lua
local a = i2c_read(0x48, 2)
local b = i2c_read(0x50, 2)
if a and b then
    set_metric("corrected_value", combine(a, b))
end
```

Each driver exposes its own named read function. All of them share one
contract: call it, get bytes back (or `nil`); whether a given function
returns immediately or actually waits for a driver task to finish is
invisible at the call site - the call above and the one below read
identically even though they behave very differently underneath.

| Function | For | Status |
|---|---|---|
| `usb_read()` | USB CDC serial | Not yet implemented *as a named function* - write `coroutine.yield()` in its place today, which returns the exact same bytes (the underlying mechanism is implemented and tested) |
| `i2c_read(addr, len)` | I2C, at the given 7-bit address | Not yet implemented (no I2C driver exists yet) |
| `dht22_read()` | DHT22/AM2302 single-wire sensor | Not yet implemented as a function - see below |

`i2c_read` takes an address because an I2C transaction always targets
one specific device on a shared bus - only the caller knows which, so it
has to be a call-time argument. `usb_read()` takes none *today* simply
because only one USB device is in scope right now; a future
`usb_read(device_id)` would only start mattering once there's more than
one to tell apart.

```lua
-- usb_read() isn't wired up as a named function yet (see the table
-- above) - for now, write coroutine.yield() in its place; it returns
-- the exact same bytes.
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

**`coroutine.yield()`/`coroutine.yield(ms)` are plain, generic
cooperative-scheduling primitives** - nothing here is specific to any
driver. They mean exactly what a yield means in any cooperatively
scheduled system: hand control to other channels for a bit, then come
back.

- `coroutine.yield(ms)`: come back in about `ms` milliseconds, with no
  data. Use it for your own timing - how often to re-check something, or
  to break a long computation into chunks so other channels still get a
  turn: `coroutine.yield(0)` comes back at the very next opportunity
  rather than parking indefinitely - the right way to "yield without
  really blocking."
- `coroutine.yield()` (bare, no value): suspends with no guarantee of
  ever being resumed on its own - safe only inside a function (like
  `usb_read()` above) that has its own separate way of making sure it
  eventually gets woken up. You shouldn't need to write this directly in
  your own script's code.

**Why DHT22 doesn't have this table's third column filled in yet**: its
single-wire protocol needs sub-100us bit timing that has to run in a
dedicated task, independent of whatever else is happening for other
channels at that moment - seeing a `dht22_read()`-shaped example here
would be misleading until that's actually wired up (see DESIGN.md §9.1
for the real constraint).

A script that errors (a runaway loop, an invalid API call, a Lua syntax
error past upload-time validation) has its channel marked `status="error"`
in `/metrics` (§2) and stops running until a corrected script is uploaded.

## 4. Uploading a script: `POST /scripts/<channel>`

- `<channel>` is 1–31 characters, no `/`.
- Request body: the Lua source, as plain text. Max 8192 bytes.
- The script is syntax-checked before anything else changes: a rejected
  upload never touches the channel's previously running script.
- Response: `200 OK` on success; `400 Bad Request` with a plain-text reason
  on a bad channel name or invalid script; `404 Not Found` for an unknown
  channel; `401 Unauthorized` if signed uploads are required (below) and the
  signature is missing or wrong.

### Optional: signed uploads

If the device was built with an authorized-uploader public key configured
(`CONFIG_UPLOAD_AUTH_ECDSA_PUBLIC_KEY`), every `POST /scripts/<channel>`
must carry two headers, obtained in order:

1. `GET /scripts/nonce` → a one-time hex nonce (returns `404` with an
   explanatory message if signing isn't required on this build — check this
   first rather than assuming).
2. Sign `SHA-256("<channel>,<nonce_hex>,<body>")` with the matching P-256
   private key, DER-encode the signature, hex-encode that. In practice:

   ```sh
   nonce=$(curl -s http://<device>/scripts/nonce)
   printf '%s,%s,%s' "<channel>" "$nonce" "$(cat script.lua)" \
     | openssl dgst -sha256 -sign key.pem | xxd -p -c 256
   ```

3. `POST /scripts/<channel>` with headers `X-Nonce: <nonce from step 1>`
   and `X-Signature: <hex signature from step 2>`.

The nonce is single-use and expires after a few minutes; fetch a fresh one
per upload attempt (including retries).

## 5. Self-documentation endpoints

| Endpoint | Content |
|---|---|
| `GET /` | This manual, rendered to HTML. Carries a `<link rel="alternate" type="text/markdown" href="/manual.md">` in `<head>`, and the same link visibly in the body, for tooling that only looks at one of the two. |
| `GET /manual.md` | This exact file, raw. |
| `GET /openapi.yaml` | OpenAPI 3.x description of `/metrics` and `/scripts/*`, with `info.externalDocs.url` pointing back at `/manual.md`. |

All three are static, pre-rendered at firmware build time from this same
source file — they describe exactly the firmware that is currently running,
never a different version.
