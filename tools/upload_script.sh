#!/usr/bin/env bash
# Uploads a Lua parser script to a channel over HTTP (DESIGN.md §10).
#
# Usage:
#   tools/upload_script.sh <host> <channel> <script-file> [private-key.pem]
#
#   <host>            board address, e.g. 192.168.49.115
#   <channel>         channel name, e.g. dev0
#   <script-file>     path to the Lua source to upload
#   [private-key.pem] PEM-encoded P-256 EC private key matching the device's
#                     CONFIG_UPLOAD_AUTH_ECDSA_PUBLIC_KEY (see that Kconfig
#                     help text for how to generate a key pair); omit if
#                     upload_auth is disabled on the device (the default -
#                     see DESIGN.md §10). Can also be supplied via the
#                     UPLOAD_AUTH_PRIVATE_KEY environment variable instead
#                     of a command-line argument.
#
# Requires curl, openssl, and xxd.

set -euo pipefail

host="${1:?usage: $0 <host> <channel> <script-file> [private-key.pem]}"
channel="${2:?usage: $0 <host> <channel> <script-file> [private-key.pem]}"
script_file="${3:?usage: $0 <host> <channel> <script-file> [private-key.pem]}"
key="${4:-${UPLOAD_AUTH_PRIVATE_KEY:-}}"

url="http://${host}/scripts/${channel}"

if [ -z "$key" ]; then
    echo "No private key given - uploading without auth headers (upload_auth disabled on device)." >&2
    curl -sS -m 10 -w '\nhttp=%{http_code}\n' -X POST --data-binary "@${script_file}" "$url"
    exit 0
fi

nonce="$(curl -sS -m 5 "http://${host}/scripts/nonce")"
if [ -z "$nonce" ]; then
    echo "Failed to fetch a nonce from http://${host}/scripts/nonce" >&2
    exit 1
fi

# Signed message is "<channel>,<nonce>,<body>" (DESIGN.md §10) - assembled
# in a temp file so the signature covers the exact uploaded bytes, no shell
# quoting surprises.
msg_file="$(mktemp)"
sig_file="$(mktemp)"
trap 'rm -f "$msg_file" "$sig_file"' EXIT
printf '%s,%s,' "$channel" "$nonce" >"$msg_file"
cat "$script_file" >>"$msg_file"

# Standard ECDSA-SHA256 over the message, DER-encoded - the same format any
# other openssl/PKI-based tool produces, and what upload_auth.c expects and
# converts internally (DESIGN.md §10).
openssl dgst -sha256 -sign "$key" -out "$sig_file" "$msg_file"
signature="$(xxd -p -c99999 "$sig_file")"

curl -sS -m 10 -w '\nhttp=%{http_code}\n' -X POST \
    -H "X-Nonce: ${nonce}" \
    -H "X-Signature: ${signature}" \
    --data-binary "@${script_file}" \
    "$url"
