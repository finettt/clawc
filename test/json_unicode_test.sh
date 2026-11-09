#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
D=$(mktemp -d /tmp/clawc-unicode-test.XXXXXX)
trap 'rm -rf "$D"' EXIT
${HOSTCC:-cc} -std=c99 -O2 -pthread -Isrc test/json_unicode_test.c src/json.c src/util.c -o "$D/test"
"$D/test"
