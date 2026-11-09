#!/bin/sh
# Check generated data and exercise every text entry under concurrency.
set -eu
cd "$(dirname "$0")/.."
D=$(mktemp -d /tmp/clawc-textpack-test.XXXXXX)
trap 'rm -rf "$D"' EXIT
${HOSTCC:-cc} -std=c99 -O2 -o "$D/gen" tools/textpack.c
"$D/gen" > "$D/data.h"
cmp src/packed_text_data.h "$D/data.h"
${HOSTCC:-cc} -std=c99 -O1 -pthread -o "$D/test" test/text_pack_test.c src/packed_text.c
"$D/test"
