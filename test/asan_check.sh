#!/usr/bin/env bash
# ASan/UBSan smoke checks. Run `make test` first to generate TLS fixtures.
set -euo pipefail
cd "$(dirname "$0")/.."
CERT=/tmp/clawc-test-tls/cert.pem
KEY=/tmp/clawc-test-tls/key.pem
WS=$(mktemp -d /tmp/clawc-asan-ws.XXXXXX)
LOG=$(mktemp /tmp/clawc-asan-log.XXXXXX)
MP=''
cleanup() {
    [ -z "$MP" ] || kill "$MP" 2>/dev/null || :
    rm -f clawc-asan "$LOG"
    rm -rf "$WS"
}
trap cleanup EXIT
[ -s "$CERT" ] && [ -s "$KEY" ] || { echo 'Run make test to generate TLS fixtures first' >&2; exit 1; }
cc -std=c99 -O1 -g -fsanitize=address,undefined -DCLAWC_TLS -o clawc-asan src/*.c -lssl -lcrypto
CLAWC_TEST_CERT="$CERT" CLAWC_TEST_KEY="$KEY" python3 test/mock_server.py 8873 8875 >"$LOG" 2>&1 &
MP=$!
sleep 1
kill -0 "$MP" 2>/dev/null || { cat "$LOG"; exit 1; }
# Fixtures use user ID 42; no implicit/persistent authorization.
export CLAWC_TELEGRAM_USERS=42
export ASAN_OPTIONS=detect_leaks=0
check_run() {
    local name=$1; shift
    : > "$LOG"
    if ! "$@" >"$LOG" 2>&1; then cat "$LOG" >&2; echo "FAIL: $name" >&2; exit 1; fi
    if grep -E 'ERROR: AddressSanitizer|SUMMARY: (AddressSanitizer|UndefinedBehaviorSanitizer)|runtime error:' "$LOG"; then
        echo "FAIL: $name sanitizer diagnostic" >&2; exit 1
    fi
    echo "ok: $name"
}
./clawc-asan -w "$WS" init >/dev/null
rm -f "$WS/history"/tg-offset-*
check_run 'Telegram HTTP' env CLAWC_TELEGRAM_API=http://127.0.0.1:8873 \
    timeout 60 ./clawc-asan -q --insecure -w "$WS" -u http://127.0.0.1:8873/v1 -k t \
    telegram -token TESTTOKEN --once -poll 1
check_run 'HTTPS fetch' timeout 60 ./clawc-asan -q --insecure -w "$WS" \
    -u https://127.0.0.1:8875/v1 -k t run 'use-fetch now'
rm -f "$WS/history"/tg-offset-*
check_run 'Telegram HTTPS' env CLAWC_TELEGRAM_API=https://127.0.0.1:8875 \
    timeout 60 ./clawc-asan -q --insecure -w "$WS" -u https://127.0.0.1:8875/v1 -k t \
    telegram -token TESTTOKEN --once -poll 1
check_run 'Chat persistence' bash -c "printf 'persist me please\\n/quit\\n' | ./clawc-asan -q --insecure -w '$WS' -u http://127.0.0.1:8873/v1 -k t chat"
