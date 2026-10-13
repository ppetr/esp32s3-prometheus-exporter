#!/usr/bin/env bash
# Uploads a Lua parser script to a channel over HTTP (DESIGN.md §10).
#
# Usage:
#   tools/upload_script.sh <host> <channel> <script-file> [hmac-key]
#
#   <host>        board address, e.g. 192.168.49.115
#   <channel>     channel name, e.g. dev0
#   <script-file> path to the Lua source to upload
#   [hmac-key]    shared secret (CONFIG_UPLOAD_AUTH_HMAC_KEY); omit if
#                 upload_auth is disabled on the device (the default - see
#                 DESIGN.md §10). Can also be supplied via the
#                 UPLOAD_AUTH_HMAC_KEY environment variable instead of a
#                 command-line argument.
#
# Requires curl and openssl.

set -euo pipefail

host="${1:?usage: $0 <host> <channel> <script-file> [hmac-key]}"
channel="${2:?usage: $0 <host> <channel> <script-file> [hmac-key]}"
script_file="${3:?usage: $0 <host> <channel> <script-file> [hmac-key]}"
key="${4:-${UPLOAD_AUTH_HMAC_KEY:-}}"

url="http://${host}/scripts/${channel}"

if [ -z "$key" ]; then
    echo "No HMAC key given - uploading without auth headers (upload_auth disabled on device)." >&2
    curl -sS -m 10 -w '\nhttp=%{http_code}\n' -X POST --data-binary "@${script_file}" "$url"
    exit 0
fi

nonce="$(curl -sS -m 5 "http://${host}/scripts/nonce")"
if [ -z "$nonce" ]; then
    echo "Failed to fetch a nonce from http://${host}/scripts/nonce" >&2
    exit 1
fi

# Signed message is "<channel>,<nonce>,<body>" (DESIGN.md §10) - assembled
# in a temp file so the HMAC covers the exact uploaded bytes, no shell
# quoting surprises.
msg_file="$(mktemp)"
trap 'rm -f "$msg_file"' EXIT
printf '%s,%s,' "$channel" "$nonce" >"$msg_file"
cat "$script_file" >>"$msg_file"

signature="$(openssl dgst -sha256 -hmac "$key" -r "$msg_file" | awk '{print $1}')"

curl -sS -m 10 -w '\nhttp=%{http_code}\n' -X POST \
    -H "X-Nonce: ${nonce}" \
    -H "X-Signature: ${signature}" \
    --data-binary "@${script_file}" \
    "$url"
