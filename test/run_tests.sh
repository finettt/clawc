#!/usr/bin/env bash
# clawc end-to-end tests: mock LLM (HTTP+HTTPS) + mock Telegram API.
set -u
cd "$(dirname "$0")/.."

PASS=0
FAIL=0
ok()   { echo "  ok: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
check() { local desc="$1"; shift; if "$@" >/dev/null 2>&1; then ok "$desc"; else fail "$desc"; fi; }

PORT=8873
TLS_PORT=8875
REQDIR=/tmp/clawc-test-reqs
WS=/tmp/clawc-test-ws
TLSDIR=/tmp/clawc-test-tls
# Deterministic fixture only. Real deployments must generate a random secret.
export CLAWC_BRIDGE_TOKEN=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
# Existing bridge tests explicitly exercise the loopback-only plaintext mode.
export CLAWC_SERVE_TLS=0 CLAWC_A2A_TLS=0
# Explicit fixture identities: channel authentication is fail-closed.
export CLAWC_TELEGRAM_USERS="42,777,888,666,300,400,701,702,703,704,444,999,778"
export CLAWC_MATRIX_USERS="@alice:localhost,@bob:localhost"
export CLAWC_DISCORD_USERS="u1,u2"
rm -rf "$REQDIR" "$WS" "$TLSDIR"
rm -f /tmp/clawc-test-port

echo "== build & size =="
CLAWC="${CLAWC:-build/native/clawc}"   # produced by `make`
[ -x "$CLAWC" ] || make
# budget build: the size contract lives on the core channel set
rm -f /tmp/clawc-budget
make -s CHANNELS=telegram TARGET=/tmp/clawc-budget >/dev/null 2>&1
BSIZE=$(stat -c %s /tmp/clawc-budget 2>/dev/null || echo 999999)
if [ "$BSIZE" -lt 102400 ]; then ok "budget build (CHANNELS=telegram) $BSIZE < 100KB"; else fail "budget build $BSIZE >= 100KB"; fi
SIZE=$(stat -c %s "$CLAWC")
if [ "$SIZE" -lt 131072 ]; then ok "full build $SIZE <= 128KB all-channels ceiling"; else fail "full build $SIZE > 128KB"; fi
if "$CLAWC" help 2>/dev/null | grep -q "telegram"; then ok "telegram command present"; else fail "telegram command present"; fi

echo "== init =="
"$CLAWC" -w "$WS" init >/dev/null
check "SOUL.md created"   test -f "$WS/SOUL.md"
check "MEMORY.md created" test -f "$WS/MEMORY.md"

echo "== TLS fixtures =="
mkdir -p "$TLSDIR"
openssl req -x509 -newkey rsa:2048 -keyout "$TLSDIR/key.pem" -out "$TLSDIR/cert.pem" \
    -days 2 -nodes -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1
check "self-signed cert generated" test -s "$TLSDIR/cert.pem"
openssl req -x509 -newkey rsa:2048 -keyout "$TLSDIR/other-key.pem" -out "$TLSDIR/other-cert.pem" \
    -days 2 -nodes -subj "/CN=other" >/dev/null 2>&1
check "independent untrusted CA generated" test -s "$TLSDIR/other-cert.pem"
openssl req -new -newkey rsa:2048 -nodes -keyout "$TLSDIR/expired-key.pem" \
    -out "$TLSDIR/expired.csr" -subj "/CN=localhost" >/dev/null 2>&1
printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\n' > "$TLSDIR/expired-ext.cnf"
openssl x509 -req -in "$TLSDIR/expired.csr" -CA "$TLSDIR/cert.pem" -CAkey "$TLSDIR/key.pem" -CAcreateserial \
    -not_before 20190101000000Z -not_after 20200101000000Z -extfile "$TLSDIR/expired-ext.cnf" \
    -out "$TLSDIR/expired.pem" >/dev/null 2>&1
check "expired certificate fixture generated" test -s "$TLSDIR/expired.pem"

echo "== jail fixtures =="
cc -o /tmp/clawc-rawsys-escape test/rawsys_escape.c 2>/dev/null
check "raw-syscall escape fixture built" test -x /tmp/clawc-rawsys-escape

echo "== mock LLM + telegram =="
# kill any stale mocks (portable: pkill -f with [r] avoids self-match; fuser may not exist)
pkill -f "mock_serve[r].py" 2>/dev/null
sleep 0.3
CLAWC_TEST_CERT="$TLSDIR/cert.pem" CLAWC_TEST_KEY="$TLSDIR/key.pem" \
    python3 test/mock_server.py $PORT $TLS_PORT >/tmp/clawc-test-mock.log 2>&1 &
MOCK_PID=$!
trap 'kill $MOCK_PID 2>/dev/null' EXIT
for i in $(seq 1 50); do
    kill -0 $MOCK_PID 2>/dev/null || { echo "  mock died:"; cat /tmp/clawc-test-mock.log; exit 1; }
    code=$(curl -s -m 1 -o /dev/null -w '%{http_code}' -X POST \
        "http://127.0.0.1:$PORT/v1/chat/completions" \
        -H 'Content-Type: application/json' \
        -d '{"tools":[{"function":{"name":"run_shell"}}],"messages":[{"role":"user","content":"ping"}]}')
    [ "$code" = "200" ] && break
    sleep 0.1
done
check "https mock up" curl -sk -m 2 -o /dev/null "https://127.0.0.1:$TLS_PORT/page"

API="http://127.0.0.1:$PORT/v1"
TAPI="http://127.0.0.1:$PORT"

echo "== openai tool round-trip (run_shell) =="
OUT=$("$CLAWC" -q -w "$WS" -u "$API" -k testkey run "please run the tool" 2>/dev/null)
case "$OUT" in
    *"FINAL:"*"hello-from-tool"*) ok "reply echoes tool output";;
    *) fail "reply echoes tool output (got: $OUT)";;
esac
check "request contains tools array" grep -q '"tools"' "$REQDIR"/req-*.json
check "request contains tool_call_id" grep -q '"tool_call_id"' "$REQDIR"/req-*.json
check "request streams (stream:true)" grep -q '"stream":true' "$REQDIR"/req-*.json

echo "== anthropic tool round-trip (load_skill) =="
OUT=$("$CLAWC" -q -w "$WS" -a anthropic -u "$API" -k testkey run "skill-test please" 2>/dev/null)
case "$OUT" in
    *"FINAL:"*"Skills load from workspace"*) ok "anthropic path works";;
    *) fail "anthropic path works (got: $OUT)";;
esac

echo "== heartbeat =="
OUT=$("$CLAWC" -q -w "$WS" -u "$API" -k testkey heartbeat 2>/dev/null)
if [ -z "$OUT" ]; then ok "HEARTBEAT_OK suppressed silently"; else fail "heartbeat output: $OUT"; fi

echo "== memory write =="
OUT=$("$CLAWC" -q -w "$WS" -u "$API" -k testkey run "use-remember now" 2>/dev/null)
check "MEMORY.md got the fact" grep -q "pizza is the favorite food" "$WS/MEMORY.md"

echo "== sandbox =="
OUT=$("$CLAWC" -q -w "$WS" -x "$WS" -u "$API" -k testkey run "use-sandbox-write now" 2>/dev/null)
case "$OUT" in
    *"path not allowed"*) ok "sandbox blocked escape";;
    *) fail "sandbox blocked escape (got: $OUT)";;
esac

echo "== landlock OS sandbox =="
rm -f /home/user/landlock-canary "$WS"/landlock-canary2
OUT=$(CLAWC_SANDBOX=landlock timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        run "use-landlock-escape now" 2>/tmp/clawc-ll-err.txt)
if grep -q "clawc jail active" /tmp/clawc-ll-err.txt; then
    # kernel without Landlock: the clawc jail fallback must take over
    check "jail fallback blocked shell escape (no landlock)" test ! -f /home/user/landlock-canary
    OUT=$(CLAWC_SANDBOX=landlock timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
            run "use-landlock-write now" 2>/dev/null)
    check "jail fallback allows workspace/tmp writes" grep -q "landlock-ok" "$WS"/landlock-canary2
    echo '{"sandbox": "landlock"}' > /tmp/clawc-ll-cfg.json
    check "jail fallback config mode accepted" bash -c ""$CLAWC" -c /tmp/clawc-ll-cfg.json config | grep -q 'sandbox: landlock (file)'"
else
    check "landlock blocked write outside workspace" test ! -f /home/user/landlock-canary
    OUT=$(CLAWC_SANDBOX=landlock timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
            run "use-landlock-write now" 2>/dev/null)
    check "landlock allows write inside workspace" grep -q "landlock-ok" "$WS"/landlock-canary2
    echo '{"sandbox": "landlock"}' > /tmp/clawc-ll-cfg.json
    check "landlock config mode accepted" bash -c ""$CLAWC" -c /tmp/clawc-ll-cfg.json config | grep -q 'sandbox: landlock (file)'"
    OUT=$(CLAWC_SANDBOX=os timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
            run "use-landlock-write now" 2>/dev/null)
    check "sandbox:os config alias works" grep -q "landlock-ok" "$WS"/landlock-canary2
fi
check "no /etc/evil created" test ! -f /etc/evil

echo "== clawc jail fallback (stub backend) =="
# SANDBOX_STUBS=1 build has no OS backend anywhere: the clawc jail (file jail
# + shell shim + syscall tripwire in <target>-jaild) must take over
# automatically and contain the shell - no silent degradation, no no-op stub.
rm -f /tmp/clawc-stubs /tmp/clawc-stubs-jail.so /tmp/clawc-stubs-jaild
make -s SANDBOX_STUBS=1 TLS=0 TARGET=/tmp/clawc-stubs >/tmp/clawc-stubs-build.log 2>&1
check "stub backend build compiles" test -x /tmp/clawc-stubs
check "jail shim builds next to it" test -s /tmp/clawc-stubs-jail.so
check "jail supervisor builds next to it" test -s /tmp/clawc-stubs-jaild
cc -std=c99 -O1 -o /tmp/clawc-jail-once test/jail_once.c src/jail.c src/util.c
rm -f /tmp/clawc-jail-once-output
if CLAWC_JAILD=/tmp/clawc-stubs-jaild /tmp/clawc-jail-once >/tmp/clawc-jail-once.log 2>&1 &&
   [ "$(wc -l </tmp/clawc-jail-once-output)" -eq 1 ]; then
    ok "jail does not retry command exiting 126"
else
    fail "jail does not retry command exiting 126"
fi
rm -f /home/user/landlock-canary
CLAWC_SANDBOX=os timeout 60 /tmp/clawc-stubs -q -w "$WS" -u "$API" -k testkey \
        run "use-landlock-escape now" >/tmp/clawc-stub-out.txt 2>/tmp/clawc-stub-err.txt
check "jail fallback activates (announced)" grep -q "clawc jail active" /tmp/clawc-stub-err.txt
check "jail shim blocked shell escape" test ! -f /home/user/landlock-canary
OUT=$(CLAWC_SANDBOX=os timeout 60 /tmp/clawc-stubs -q -w "$WS" -u "$API" -k testkey \
        run "use-jail-rawsys now" 2>/dev/null)
check "jail tripwire blocked raw-syscall escape" test ! -f /home/user/landlock-canary
check "jail tripwire logged the block" bash -c "echo \"$OUT\" | grep -q 'blocked /home/user/landlock-canary'"
OUT=$(CLAWC_SANDBOX=os timeout 60 /tmp/clawc-stubs -q -w "$WS" -u "$API" -k testkey \
        run "use-landlock-write now" 2>/dev/null)
check "jail allows workspace/tmp writes" grep -q "landlock-ok" "$WS"/landlock-canary2
OUT=$(CLAWC_SANDBOX=os timeout 60 /tmp/clawc-stubs -q -w "$WS" -u "$API" -k testkey \
        run "use-sandbox-write now" 2>/dev/null)
case "$OUT" in
    *"path not allowed"*) ok "jail file tools confined to workspace";;
    *) fail "jail file tools confined to workspace (got: $OUT)";;
esac
OUT=$(/tmp/clawc-stubs -q -w "$WS" -u "$API" -k testkey run "please run the tool" 2>/dev/null)
case "$OUT" in
    *"hello-from-tool"*) ok "stub build runs normally without sandbox mode";;
    *) fail "stub build runs normally without sandbox mode (got: $OUT)";;
esac
check "stub doctor reports no OS sandbox" bash -c "/tmp/clawc-stubs doctor 2>/dev/null | grep -q 'no OS sandbox on this platform'"
check "no /etc/evil created (jail)" test ! -f /etc/evil

echo "== path sandbox hardening =="
ln -sf /home/user "$WS"/link-out
echo hi > "$WS"/real-target.txt
ln -sf real-target.txt "$WS"/link-in
OUT=$("$CLAWC" -q -w "$WS" -x "$WS" -u "$API" -k testkey run "use-sandbox-symlink-out now" 2>/dev/null)
case "$OUT" in
    *"path not allowed"*) ok "path mode blocks symlinked dir escape";;
    *) fail "path mode blocks symlinked dir escape (got: $OUT)";;
esac
OUT=$("$CLAWC" -q -w "$WS" -x "$WS" -u "$API" -k testkey run "use-sandbox-symlink-in now" 2>/dev/null)
case "$OUT" in
    *"path not allowed"*) ok "path mode does not follow symlinks";;
    *) fail "path mode does not follow symlinks (got: $OUT)";;
esac
check "symlink target not modified" grep -q '^hi$' "$WS"/real-target.txt
check "no /home/user/evil created" test ! -f /home/user/evil

echo "== fetch_url tool =="
OUT=$("$CLAWC" -q -w "$WS" -u "$API" -k testkey run "use-fetch now" 2>/dev/null)
case "$OUT" in
    *"hello web page"*) ok "fetch_url tool fetched page";;
    *) fail "fetch_url tool (got: $OUT)";;
esac

echo "== built-in HTTPS: agent over TLS =="
OUT=$("$CLAWC" -q --insecure -w "$WS" -u "https://127.0.0.1:$TLS_PORT/v1" -k testkey run "tls tool" 2>/dev/null)
case "$OUT" in
    *"FINAL:"*"hello-from-tool"*) ok "LLM round-trip over in-process TLS";;
    *) fail "LLM over TLS (got: $OUT)";;
esac

echo "== built-in HTTPS: cert verification =="
OUT=$("$CLAWC" -q -w "$WS" -u "https://127.0.0.1:$TLS_PORT/v1" -k testkey run "tls tool" 2>/dev/null)
case "$OUT" in
    *"TLS"*|*"verify"*|*"certificate"*) ok "self-signed cert rejected without --insecure";;
    *) fail "cert not rejected (got: $OUT)";;
esac

echo "== telegram channel =="
rm -f "$REQDIR"/req-*.json
rm -f "$WS/history"/tg-offset-*
OUT=$(CLAWC_TELEGRAM_API="$TAPI" timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        telegram -token TESTTOKEN --once -poll 1 2>/dev/null)
check "sendMessage captured" grep -q '"chat_id":42' "$REQDIR"/req-*.json
check "agent reply delivered to telegram" grep -q '"text":"FINAL' "$REQDIR"/req-*.json
check "working message edited with the reply" grep -q '"message_id":[0-9]*,"text":"FINAL' "$REQDIR"/req-*.json
check "no checkmark status message" bash -c '! grep -q "✓" '"$REQDIR"'/req-*.json'
check "getUpdates polled with offset" grep -q '"offset"' "$REQDIR"/req-*.json

echo "== telegram over HTTPS with --insecure =="
rm -f "$REQDIR"/req-*.json
rm -f "$WS/history"/tg-offset-*
OUT=$(CLAWC_TELEGRAM_API="https://127.0.0.1:$TLS_PORT" timeout 60 "$CLAWC" -q --insecure -w "$WS" -u "$API" -k testkey \
        telegram -token TESTTOKEN --once -poll 1 2>/dev/null)
check "telegram TLS sendMessage captured" grep -q '"chat_id":42' "$REQDIR"/req-*.json

echo "== confirmation via inline buttons (approve) =="
rm -f "$REQDIR"/req-*.json
mkdir -p /tmp/clawc-risky-test && touch /tmp/clawc-risky-test/marker
rm -f "$WS/history"/tg-offset-*
CLAWC_TELEGRAM_API="$TAPI" timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        telegram -token CONFIRMTOKEN --once -poll 1 >/dev/null 2>&1
check "confirm keyboard offered" grep -q '"callback_data":"conf:yes"' "$REQDIR"/req-*.json
check "button labels use emoji escapes" grep -q 'u2705 Approve' "$REQDIR"/req-*.json
check "button press acknowledged" grep -q '"callback_query_id"' "$REQDIR"/req-*.json
check "approved action ran (dir removed)" bash -c "test ! -e /tmp/clawc-risky-test"
check "audit records the risky call" grep -q "rm -rf /tmp/clawc-risky-test" "$WS/history/audit.jsonl"

echo "== confirmation via inline buttons (decline) =="
rm -f "$REQDIR"/req-*.json
mkdir -p /tmp/clawc-risky-test && touch /tmp/clawc-risky-test/marker
rm -f "$WS/history"/tg-offset-*
CLAWC_TELEGRAM_API="$TAPI" timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        telegram -token CONFIRMNOTOKEN --once -poll 1 >/dev/null 2>&1
check "declined action did not run" test -f /tmp/clawc-risky-test/marker
check "audit shows refusal" grep -q "refused" "$WS/history/audit.jsonl"
rm -rf /tmp/clawc-risky-test

echo "== confirmation via /yes text (fallback) =="
mkdir -p /tmp/clawc-risky-test && touch /tmp/clawc-risky-test/marker
rm -f "$WS/history"/tg-offset-*
CLAWC_TELEGRAM_API="$TAPI" timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        telegram -token CONFIRMTEXTTOKEN --once -poll 1 >/dev/null 2>&1
check "/yes text approved the action" bash -c "test ! -e /tmp/clawc-risky-test"

echo "== confirmation rejects stale button press =="
mkdir -p /tmp/clawc-risky-test && touch /tmp/clawc-risky-test/marker
NREF=$(grep -c "refused" "$WS/history/audit.jsonl" 2>/dev/null || echo 0)
rm -f "$WS/history"/tg-offset-*
CLAWC_TELEGRAM_API="$TAPI" CLAWC_CONFIRM_SECS=3 timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        telegram -token CONFIRMSTALETOKEN --once -poll 1 >/dev/null 2>&1
check "stale press did not approve" test -f /tmp/clawc-risky-test/marker
check "stale press acked expired" grep -q '"text":"expired"' "$REQDIR"/req-*.json
NREF2=$(grep -c "refused" "$WS/history/audit.jsonl" 2>/dev/null || echo 0)
if [ "$NREF2" -gt "$NREF" ]; then ok "stale confirm refused in audit"; else fail "stale confirm audit refusal ($NREF -> $NREF2)"; fi
rm -rf /tmp/clawc-risky-test

echo "== confirmation rejects unverifiable button press =="
mkdir -p /tmp/clawc-risky-test && touch /tmp/clawc-risky-test/marker
NREF=$(grep -c "refused" "$WS/history/audit.jsonl" 2>/dev/null || echo 0)
rm -f "$WS/history"/tg-offset-*
CLAWC_TELEGRAM_API="$TAPI" CLAWC_CONFIRM_SECS=3 timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        telegram -token CONFIRMNOMIDTOKEN --once -poll 1 >/dev/null 2>&1
check "unverifiable press did not approve" test -f /tmp/clawc-risky-test/marker
check "unverifiable press acked expired" grep -q '"text":"expired"' "$REQDIR"/req-*.json
NREF2=$(grep -c "refused" "$WS/history/audit.jsonl" 2>/dev/null || echo 0)
if [ "$NREF2" -gt "$NREF" ]; then ok "unverifiable confirm refused in audit"; else fail "unverifiable confirm audit ($NREF -> $NREF2)"; fi
rm -rf /tmp/clawc-risky-test

echo "== risky tool refused in heartbeat =="
mkdir -p /tmp/clawc-risky-test && touch /tmp/clawc-risky-test/marker
echo "- [ ] run the command rm -rf /tmp/clawc-risky-test" > "$WS/HEARTBEAT.md"
OUT=$("$CLAWC" -q -w "$WS" -u "$API" -k testkey heartbeat 2>/dev/null)
case "$OUT" in
    *"refused"*) ok "heartbeat refused risky tool";;
    *) fail "heartbeat refusal (got: $OUT)";;
esac
check "file survived heartbeat" test -f /tmp/clawc-risky-test/marker

echo "== --yes auto-approves =="
mkdir -p /tmp/clawc-risky-test && touch /tmp/clawc-risky-test/marker
rm -f "$WS/history"/tg-offset-*
CLAWC_TELEGRAM_API="$TAPI" timeout 60 "$CLAWC" -q --yes -w "$WS" -u "$API" -k testkey \
        telegram -token CONFIRMTOKEN --once -poll 1 >/dev/null 2>&1
check "auto-approved and executed" bash -c "test ! -e /tmp/clawc-risky-test"

echo "== /status command =="
rm -f "$REQDIR"/req-*.json
rm -f "$WS/history"/tg-offset-*
CLAWC_TELEGRAM_API="$TAPI" timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        telegram -token STATUSTOKEN --once -poll 1 >/dev/null 2>&1
check "/status replies with model info" grep -q 'clawc 0' "$REQDIR"/req-*.json
check "/status needs no LLM call" bash -c '! grep -q "\"messages\"" '"$REQDIR"'/req-*.json'

echo "== history persistence (chat) =="
rm -f "$REQDIR"/req-*.json
printf 'first turn alpha\n/quit\n' | "$CLAWC" -q -w "$WS" -u "$API" -k testkey chat >/dev/null 2>&1
check "history file created" test -s "$WS/history/chat.jsonl"
rm -f "$REQDIR"/req-*.json
printf 'second turn beta\n/quit\n' | "$CLAWC" -q -w "$WS" -u "$API" -k testkey chat >/dev/null 2>&1
LAST=$(ls "$REQDIR"/req-*.json | sort -V | tail -1)
check "prior turn restored into request" grep -q "first turn alpha" "$LAST"
check "new turn present" grep -q "second turn beta" "$LAST"
rm -f "$REQDIR"/req-*.json
printf '/reset\ngamma after reset\n/quit\n' | "$CLAWC" -q -w "$WS" -u "$API" -k testkey chat >/dev/null 2>&1
LAST=$(ls "$REQDIR"/req-*.json | sort -V | tail -1)
check "reset cleared history" bash -c "! grep -q 'first turn alpha' '$LAST'"

echo "== history sanitization =="
rm -f "$REQDIR"/req-*.json
cat > "$WS/history/chat.jsonl" <<'EOF'
{"role":"user","content":"valid earlier message"}
{"role":"tool","content":"orphan tool output zzz","tool_call_id":"nobody"}
{"role":"assistant","content":"hi there","tool_calls":[{"id":"c9","name":"run_shell","args":{"command":"echo unprompted"}}]}
EOF
printf 'new message now\n/quit\n' | "$CLAWC" -q -w "$WS" -u "$API" -k testkey chat >/dev/null 2>&1
LAST=$(grep -l '"messages"' "$REQDIR"/req-*.json | sort -V | tail -1)
check "orphan tool result dropped" bash -c "! grep -q 'orphan tool output zzz' '$LAST'"
check "unanswered tool_calls stripped" bash -c "! grep -q 'unprompted' '$LAST'"
check "valid history kept" grep -q "valid earlier message" "$LAST"

echo "== audit log =="
check "audit.jsonl exists" test -s "$WS/history/audit.jsonl"
check "audit records tool calls" grep -q '"tool":"run_shell"' "$WS/history/audit.jsonl"

echo "== serve per-user isolation =="
"$CLAWC" -q -w "$WS" -u "$API" -k testkey serve -p 8876 &
SERVE_PID=$!
sleep 0.5
python3 - <<'EOF'
import json, os, socket
def send(obj):
    s = socket.create_connection(("127.0.0.1", 8876), timeout=30)
    req = json.loads(obj)
    req["token"] = os.environ["CLAWC_BRIDGE_TOKEN"]
    s.sendall(json.dumps(req).encode() + b"\n")
    buf = b""
    while not buf.endswith(b"\n"):
        d = s.recv(4096)
        if not d: break
        buf += d
    s.close()
send(b'{"from":"alice","text":"alice secret one"}')
send(b'{"from":"bob","text":"bob hello there"}')
send(b'{"from":"alice","text":"alice again now"}')
EOF
kill $SERVE_PID 2>/dev/null
LAST=$(ls "$REQDIR"/req-*.json | sort -V | tail -1)
check "alice history kept" grep -q "alice secret one" "$LAST"
check "alice new turn kept" grep -q "alice again now" "$LAST"
check "bob's messages not in alice's context" bash -c "! grep -q 'bob hello there' '$LAST'"

echo "== telegram session persistence =="
rm -f "$REQDIR"/req-*.json
rm -f "$WS/history"/tg-offset-*
CLAWC_TELEGRAM_API="$TAPI" timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        telegram -token TESTTOKEN --once -poll 1 >/dev/null 2>&1
CLAWC_TELEGRAM_API="$TAPI" timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        telegram -token TESTTOKEN --once -poll 1 >/dev/null 2>&1
LAST=$(grep -l '"messages"' "$REQDIR"/req-*.json | sort -V | tail -1)
check "prior turn restored into context" grep -q "hello telegram" "$LAST"
check "new turn present after resume" grep -q "hello again telegram" "$LAST"

echo "== telegram update offset persistence =="
rm -f "$REQDIR"/req-*.json
rm -f "$WS/history"/tg-offset-*
CLAWC_TELEGRAM_API="$TAPI" timeout 20 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        telegram -token TESTTOKEN --once -poll 1 >/dev/null 2>&1
check "offset file created" bash -c "grep -q '^101\$' $WS/history/tg-offset-*"
rm -f "$REQDIR"/req-*.json
CLAWC_TELEGRAM_API="$TAPI" timeout 20 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        telegram -token TESTTOKEN --once -poll 1 >/dev/null 2>&1
check "restart resumes at saved offset" grep -q '"offset":101' "$REQDIR"/req-*.json
LAST=$(grep -l '"messages"' "$REQDIR"/req-*.json | sort -V | tail -1)
check "consumed message not replayed" grep -q "hello again telegram" "$LAST"
check "offset advanced to next update" bash -c "grep -q '^201\$' $WS/history/tg-offset-*"

echo "== context compaction =="
rm -f "$REQDIR"/req-*.json
rm -f "$WS/history"/tg-offset-*
CLAWC_TELEGRAM_API="$TAPI" timeout 20 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        telegram -token TESTTOKEN --once -poll 1 >/dev/null 2>&1
R1=$(grep -l '"messages"' "$REQDIR"/req-*.json | sort -V | tail -1)
SYS=$(python3 -c "import json;d=json.load(open('$R1'));print(len(d['messages'][0].get('content') or ''))")
rm -f "$REQDIR"/req-*.json
CLAWC_TELEGRAM_API="$TAPI" CLAWC_CONTEXT_CHARS=$((SYS+200)) timeout 20 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        telegram -token TESTTOKEN --once -poll 1 >/dev/null 2>&1
LAST=$(grep -l '"messages"' "$REQDIR"/req-*.json | sort -V | tail -1)
check "new turn kept after compaction" grep -q "hello again telegram" "$LAST"
check "old turn compacted away" bash -c "! grep -q 'FINAL: hello-from-tool' '$LAST'"
check "compaction summary in request" grep -q "compacted earlier conversation" "$LAST"

echo "== serve mode =="
check "serve refuses missing bridge token" bash -c 'unset CLAWC_BRIDGE_TOKEN; "$CLAWC" serve -p 8874 >/dev/null 2>&1; test $? -ne 0'
check "serve refuses malformed bridge token" bash -c 'CLAWC_BRIDGE_TOKEN=short "$CLAWC" serve -p 8874 >/dev/null 2>&1; test $? -ne 0'
"$CLAWC" -q -w "$WS" -u "$API" -k testkey serve -p 8874 2>/tmp/clawc-serve-bind.log &
SERVE_PID=$!
sleep 0.5
REPLY=$(python3 - <<'EOF'
import json, os, socket
s = socket.create_connection(("127.0.0.1", 8874), timeout=30)
s.sendall(json.dumps({"text":"ping via bridge", "token":os.environ["CLAWC_BRIDGE_TOKEN"]}).encode()+b"\n")
buf = b""
while not buf.endswith(b"\n"):
    d = s.recv(4096)
    if not d: break
    buf += d
print(buf.decode())
EOF
)
check "serve rejects missing and incorrect token" python3 -c '
import json, os, socket
for token in (None, "f" * 64, "short", 123):
    s = socket.create_connection(("127.0.0.1", 8874), timeout=5)
    req = {"text":"auth-negative-test"}
    if token is not None: req["token"] = token
    s.sendall(json.dumps(req).encode()+b"\n")
    data = b""
    while not data.endswith(b"\n"):
        chunk = s.recv(1024)
        if not chunk: break
        data += chunk
    s.close()
    assert json.loads(data) == {"ok":False,"error":"unauthorized"}, data
'
check "token redacted in config" bash -c 'out=$("$CLAWC" config); [[ "$out" == *"bridge_token: (set)"* && "$out" != *"$CLAWC_BRIDGE_TOKEN"* ]]'
check "unauthorized input does not reach LLM" bash -c '! grep -q "auth-negative-test" "$1"/req-*.json 2>/dev/null' -- "$REQDIR"
printf '{"bridge_token":"%s"}\n' "$CLAWC_BRIDGE_TOKEN" > /tmp/clawc-bridge-config.json
check "bridge token loads from JSON config" bash -c 'unset CLAWC_BRIDGE_TOKEN; "$CLAWC" -c /tmp/clawc-bridge-config.json config | grep -q "bridge_token: (set) (file)"'
check "serve rejects incomplete JSON line" python3 -c '
import socket
s=socket.create_connection(("127.0.0.1",8874),timeout=3)
s.sendall(b"{\"text\":\"partial\"}"); s.shutdown(socket.SHUT_WR)
assert s.recv(1024)==b""
'
check "serve releases stalled client" python3 -c '
import socket, time
s=socket.create_connection(("127.0.0.1",8874),timeout=8)
t=time.monotonic(); assert s.recv(1)==b""; assert time.monotonic()-t < 7
'
kill $SERVE_PID 2>/dev/null
case "$REPLY" in
    *'"ok":true'*"hello-from-tool"*) ok "serve round-trip";;
    *) fail "serve round-trip (got: $REPLY)";;
esac
check "serve binds loopback by default" grep -q "listening on 127.0.0.1:8874" /tmp/clawc-serve-bind.log
check "plaintext serve refuses non-loopback bind" bash -c '"$CLAWC" -q -w "$1" -u "$2" -k testkey serve -p 8881 -b 0.0.0.0 >/dev/null 2>&1; test $? -ne 0' -- "$WS" "$API"

echo "== TLS bridge =="
check "TLS serve refuses missing certificate" bash -c 'CLAWC_SERVE_TLS=1 CLAWC_SERVE_CERT= CLAWC_SERVE_KEY= "$CLAWC" serve -p 8883 >/dev/null 2>&1; test $? -ne 0'
check "TLS serve rejects mismatched key" bash -c 'CLAWC_SERVE_TLS=1 CLAWC_SERVE_CERT="$1" CLAWC_SERVE_KEY="$2" "$CLAWC" serve -p 8883 >/dev/null 2>&1; test $? -ne 0' -- "$TLSDIR/cert.pem" "$TLSDIR/other-key.pem"
printf '{}\n' >/tmp/clawc-tls-default.json
check "TLS is default for serve" bash -c 'unset CLAWC_SERVE_TLS CLAWC_SERVE_CERT CLAWC_SERVE_KEY; "$CLAWC" -c /tmp/clawc-tls-default.json serve -p 8883 >/dev/null 2>&1; test $? -ne 0'
CLAWC_SERVE_TLS=1 CLAWC_SERVE_CERT="$TLSDIR/cert.pem" CLAWC_SERVE_KEY="$TLSDIR/key.pem" \
    "$CLAWC" -q -w "$WS" -u "$API" -k testkey serve -p 8883 -b 0.0.0.0 >/tmp/clawc-bridge-tls.log 2>&1 &
TLS_BRIDGE_PID=$!
sleep 0.5
check "TLS listener starts" bash -c 'kill -0 "$1" && grep -q "listening on 0.0.0.0:8883" /tmp/clawc-bridge-tls.log' -- "$TLS_BRIDGE_PID"
check "TLS bridge accepts verified certificate and token" python3 test/bridge_tls_client.py success "$TLSDIR/cert.pem" 8883
check "TLS bridge rejects wrong token" python3 test/bridge_tls_client.py wrong-token "$TLSDIR/cert.pem" 8883
check "TLS client rejects wrong hostname" python3 test/bridge_tls_client.py wrong-host "$TLSDIR/cert.pem" 8883
check "TLS client rejects untrusted CA" python3 test/bridge_tls_client.py wrong-ca "$TLSDIR/other-cert.pem" 8883
check "TLS listener rejects plaintext" python3 test/bridge_tls_client.py plaintext "$TLSDIR/cert.pem" 8883
OUT=$(CLAWC_A2A_TLS=1 CLAWC_A2A_CA="$TLSDIR/cert.pem" CLAWC_A2A_PEERS="buddy=127.0.0.1:8883" \
    timeout 90 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "use-a2a now" 2>/dev/null)
case "$OUT" in *mango-delivered*) ok "A2A verified TLS round trip";; *) fail "A2A verified TLS round trip (got: $OUT)";; esac
OUT=$(CLAWC_A2A_TLS=1 CLAWC_A2A_CA="$TLSDIR/cert.pem" CLAWC_A2A_PEERS="buddy=127.0.0.2:8883" \
    timeout 90 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "use-a2a now" 2>/dev/null)
case "$OUT" in *"TLS verification or handshake failed"*) ok "A2A rejects wrong hostname";; *) fail "A2A rejects wrong hostname (got: $OUT)";; esac
OUT=$(CLAWC_A2A_TLS=1 CLAWC_A2A_CA="$TLSDIR/other-cert.pem" CLAWC_A2A_PEERS="buddy=127.0.0.1:8883" \
    timeout 90 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "use-a2a now" 2>/dev/null)
case "$OUT" in *"TLS verification or handshake failed"*) ok "A2A rejects untrusted certificate";; *) fail "A2A rejects untrusted certificate (got: $OUT)";; esac
OUT=$(CLAWC_INSECURE=1 CLAWC_A2A_TLS=1 CLAWC_A2A_CA="$TLSDIR/other-cert.pem" CLAWC_A2A_PEERS="buddy=127.0.0.1:8883" \
    timeout 90 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "use-a2a now" 2>/dev/null)
case "$OUT" in *"TLS verification or handshake failed"*) ok "A2A ignores insecure TLS bypass";; *) fail "A2A ignores insecure TLS bypass (got: $OUT)";; esac
kill "$TLS_BRIDGE_PID" 2>/dev/null
CLAWC_SERVE_TLS=1 CLAWC_SERVE_CERT="$TLSDIR/expired.pem" CLAWC_SERVE_KEY="$TLSDIR/expired-key.pem" \
    "$CLAWC" -q -w "$WS" -u "$API" -k testkey serve -p 8885 >/tmp/clawc-expired-tls.log 2>&1 &
EXPIRED_PID=$!
sleep 0.5
check "expired certificate server starts" bash -c 'kill -0 "$1" && grep -q "listening on 127.0.0.1:8885" /tmp/clawc-expired-tls.log' -- "$EXPIRED_PID"
OUT=$(CLAWC_A2A_TLS=1 CLAWC_A2A_CA="$TLSDIR/cert.pem" CLAWC_A2A_PEERS="buddy=127.0.0.1:8885" \
    timeout 90 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "use-a2a now" 2>/dev/null)
case "$OUT" in *"TLS verification or handshake failed"*) ok "A2A rejects expired certificate";; *) fail "A2A rejects expired certificate (got: $OUT)";; esac
kill "$EXPIRED_PID" 2>/dev/null

rm -f /tmp/clawc-tls-capture
python3 test/bridge_capture.py 8884 /tmp/clawc-tls-capture &
CAPTURE_PID=$!
sleep 0.3
OUT=$(CLAWC_A2A_TLS=1 CLAWC_A2A_CA="$TLSDIR/cert.pem" CLAWC_A2A_PEERS="buddy=127.0.0.1:8884" \
    timeout 90 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "use-a2a now" 2>/dev/null)
wait "$CAPTURE_PID" 2>/dev/null
case "$OUT" in *"TLS verification or handshake failed"*) ok "A2A does not downgrade to plaintext";; *) fail "A2A does not downgrade to plaintext (got: $OUT)";; esac
check "failed TLS handshake does not reveal bearer token" bash -c 'test -s /tmp/clawc-tls-capture && ! grep -q "$CLAWC_BRIDGE_TOKEN" /tmp/clawc-tls-capture'
OUT=$(CLAWC_A2A_TLS=0 CLAWC_A2A_PEERS="buddy=192.0.2.1:8884" \
    timeout 90 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "use-a2a now" 2>/dev/null)
case "$OUT" in *"plaintext is loopback-only"*) ok "plaintext A2A rejects remote peer";; *) fail "plaintext A2A rejects remote peer (got: $OUT)";; esac
rm -f /tmp/clawc-no-tls
make -s TLS=0 TARGET=/tmp/clawc-no-tls >/tmp/clawc-no-tls-build.log 2>&1
check "TLS=0 build compiles" test -x /tmp/clawc-no-tls
check "TLS=0 does not silently accept TLS serve" bash -c 'CLAWC_SERVE_TLS=1 CLAWC_SERVE_CERT="$1" CLAWC_SERVE_KEY="$2" /tmp/clawc-no-tls serve -p 8883 >/dev/null 2>&1; test $? -ne 0' -- "$TLSDIR/cert.pem" "$TLSDIR/key.pem"
CLAWC_SERVE_TLS=0 /tmp/clawc-no-tls -q -w "$WS" -u "$API" -k testkey serve -p 8886 >/tmp/clawc-no-tls-serve.log 2>&1 &
NO_TLS_PID=$!
sleep 0.5
check "TLS=0 plaintext loopback remains usable" python3 -c '
import json, os, socket
s=socket.create_connection(("127.0.0.1",8886),timeout=10)
s.sendall(json.dumps({"text":"ping via bridge", "token":os.environ["CLAWC_BRIDGE_TOKEN"]}).encode()+b"\n")
data=b""
while not data.endswith(b"\n"):
    part=s.recv(4096)
    if not part: break
    data += part
s.close()
r=json.loads(data)
assert r.get("ok") and "hello-from-tool" in r.get("reply", ""), r
'
OUT=$(CLAWC_A2A_TLS=1 CLAWC_A2A_PEERS="buddy=127.0.0.1:8886" \
    timeout 90 /tmp/clawc-no-tls -q -w "$WS" -u "$API" -k testkey run "use-a2a now" 2>/dev/null)
case "$OUT" in *"peer TLS unavailable in TLS=0 build"*) ok "TLS=0 A2A refuses TLS instead of downgrading";; *) fail "TLS=0 A2A refuses TLS instead of downgrading (got: $OUT)";; esac
kill "$NO_TLS_PID" 2>/dev/null

echo "== run_shell tool actually runs =="
OUT=$("$CLAWC" -q -w "$WS" -u "$API" -k testkey run "just do it" 2>/dev/null)
case "$OUT" in
    *"hello-from-tool"*) ok "shell executed and output returned";;
    *) fail "shell executed (got: $OUT)";;
esac

echo "== malformed tool args feedback =="
OUT=$(timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "use-bad-args now" 2>/dev/null)
check "model told to re-issue bad call" grep -q "malformed JSON arguments" "$WS/history/audit.jsonl"

echo "== discord channel =="
rm -f "$REQDIR"/req-*.json
CLAWC_DISCORD_TOKEN=DCTEST CLAWC_DISCORD_GATEWAY="ws://127.0.0.1:$((PORT+100))" \
        CLAWC_DISCORD_API="$TAPI" timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        discord >/dev/null 2>&1
check "discord round trip answered" grep -q "hello discord" "$REQDIR"/req-*.json
check "discord reply posted" bash -c "grep -q '\"content\"' '$REQDIR'/req-*.json && grep -q FINAL '$REQDIR'/req-*.json"
OUT=$(CLAWC_DISCORD_TOKEN=X "$CLAWC" config)
case "$OUT" in
    *"gateways:"*"discord"*) ok "discord gateway planned";;
    *) fail "discord gateway plan (got: $OUT)";;
esac

echo "== discord rejects bot messages =="
rm -f "$REQDIR"/req-*.json
CLAWC_DISCORD_TOKEN=DCBOT CLAWC_DISCORD_GATEWAY="ws://127.0.0.1:$((PORT+100))" \
        CLAWC_DISCORD_API="$TAPI" timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        discord >/dev/null 2>&1
if grep -q 'hello discord' "$REQDIR"/req-*.json 2>/dev/null; then
    fail "discord ignores bot message"
else
    ok "discord ignores bot message"
fi

echo "== discord v2: buttons, slash =="
mkdir -p /tmp/clawc-risky-test && touch /tmp/clawc-risky-test/marker
rm -f "$REQDIR"/req-*.json
CLAWC_DISCORD_TOKEN=DCFLOW CLAWC_DISCORD_GATEWAY="ws://127.0.0.1:$((PORT+100))" \
        CLAWC_DISCORD_API="$TAPI" timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        discord >/dev/null 2>&1
check "discord button approve ran action" bash -c "test ! -f /tmp/clawc-risky-test/marker"
check "discord prompt edited in place" grep -q "(approved" "$REQDIR"/req-*.json
rm -f /tmp/clawc-risky-test && mkdir -p /tmp/clawc-risky-test && touch /tmp/clawc-risky-test/marker
rm -f "$REQDIR"/req-*.json
CLAWC_DISCORD_TOKEN=DCSTALE CLAWC_DISCORD_GATEWAY="ws://127.0.0.1:$((PORT+100))" \
        CLAWC_DISCORD_API="$TAPI" CLAWC_CONFIRM_SECS=3 timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        discord >/dev/null 2>&1
check "discord stale button ignored" test -f /tmp/clawc-risky-test/marker
check "discord stale button acked expired" grep -q "(expired)" "$REQDIR"/req-*.json
rm -rf /tmp/clawc-risky-test
rm -f "$REQDIR"/req-*.json
CLAWC_DISCORD_TOKEN=DCSLASH CLAWC_DISCORD_GATEWAY="ws://127.0.0.1:$((PORT+100))" \
        CLAWC_DISCORD_API="$TAPI" timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        discord >/dev/null 2>&1
check "discord slash command ran task" grep -q "mango-delivered" "$REQDIR"/req-*.json
rm -f "$REQDIR"/req-*.json
CLAWC_DISCORD_TOKEN=DCSLASH CLAWC_DISCORD_CHANNELS=999 CLAWC_DISCORD_GATEWAY="ws://127.0.0.1:$((PORT+100))" \
        CLAWC_DISCORD_API="$TAPI" timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        discord >/dev/null 2>&1
if grep -q 'mango-delivered' "$REQDIR"/req-*.json 2>/dev/null; then
    fail "discord slash from disallowed channel rejected"
else
    ok "discord slash from disallowed channel rejected"
fi

echo "== discord media =="
rm -f "$REQDIR"/req-*.json
rm -rf "$WS/media"
CLAWC_DISCORD_TOKEN=DCMEDIA CLAWC_DISCORD_GATEWAY="ws://127.0.0.1:$((PORT+100))" \
        CLAWC_DISCORD_API="$TAPI" timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        discord >/dev/null 2>&1
check "discord image saved to media" test -f "$WS/media/test-pic.png"
check "discord file attachment saved" test -f "$WS/media/note.txt"
check "discord attachment bytes preserved" grep -q "mock-file:note.txt" "$WS/media/note.txt"
check "discord vision image in llm request" grep -q "data:image/png;base64" "$REQDIR"/req-*.json
check "discord caption reached the agent" grep -q "what is this picture" "$REQDIR"/req-*.json
rm -f "$REQDIR"/req-*.json
CLAWC_DISCORD_TOKEN=DCMEDIA2 CLAWC_DISCORD_GATEWAY="ws://127.0.0.1:$((PORT+100))" \
        CLAWC_DISCORD_API="$TAPI" timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        discord >/dev/null 2>&1
check "discord empty-content attachment handled" bash -c "test -f '$WS/media/solo.bin' && grep -q 'solo.bin' '$REQDIR'/req-*.json"

echo "== a2a peer link =="
rm -rf /tmp/clawc-a2a-peer && mkdir -p /tmp/clawc-a2a-peer
"$CLAWC" -q -w /tmp/clawc-a2a-peer -u "$API" -k testkey serve -p 8882 >/dev/null 2>&1 &
PEER_PID=$!
sleep 0.5
OUT=$(CLAWC_A2A_PEERS="buddy=127.0.0.1:8882" timeout 90 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "use-a2a now" 2>/dev/null)
case "$OUT" in
    *mango-delivered*) ok "a2a named peer round trip";;
    *) fail "a2a round trip (got: $OUT)";;
esac
OUT=$(CLAWC_BRIDGE_TOKEN=ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff CLAWC_A2A_PEERS="buddy=127.0.0.1:8882" timeout 90 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "use-a2a now" 2>/dev/null)
case "$OUT" in
    *"peer authentication failed"*) ok "a2a wrong token rejected";;
    *) fail "a2a wrong token rejected (got: $OUT)";;
esac
OUT=$(timeout 90 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "use-a2a-bad now" 2>/dev/null)
case "$OUT" in
    *"unknown peer"*) ok "a2a unconfigured peer rejected";;
    *) fail "a2a bad peer (got: $OUT)";;
esac
kill $PEER_PID 2>/dev/null

echo "== matrix channel =="
rm -f "$WS/history"/matrix-sync-*
rm -f "$REQDIR"/req-*.json
CLAWC_MATRIX_HOMESERVER="$TAPI" CLAWC_MATRIX_TOKEN=MXTEST timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey matrix >/dev/null 2>&1
check "matrix round trip answered" grep -q "hello matrix" "$REQDIR"/req-*.json
check "matrix reply sent to room" bash -c "grep -q '\"msgtype\":\"m.text\"' '$REQDIR'/req-*.json && grep -q 'FINAL' '$REQDIR'/req-*.json"
check "matrix cursor persisted" bash -c "grep -q '^s1' $WS/history/matrix-sync-*"
rm -f "$REQDIR"/req-*.json
CLAWC_MATRIX_HOMESERVER="$TAPI" CLAWC_MATRIX_TOKEN=MXTEST timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey matrix >/dev/null 2>&1
check "matrix no replay after restart" bash -c "! grep -q 'hello matrix' '$REQDIR'/req-*.json"
rm -f "$WS/history"/matrix-sync-* "$REQDIR"/req-*.json
CLAWC_MATRIX_HOMESERVER="$TAPI" CLAWC_MATRIX_TOKEN=MXROOMS CLAWC_MATRIX_ROOMS='!ok:localhost' \
        timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey matrix >/dev/null 2>&1
check "matrix room allowlist answered" grep -q "hello allowed" "$REQDIR"/req-*.json
check "matrix room allowlist ignored others" bash -c "! grep -q 'ignore me' '$REQDIR'/req-*.json"
OUT=$(CLAWC_MATRIX_HOMESERVER="$TAPI" CLAWC_MATRIX_TOKEN=X "$CLAWC" config)
case "$OUT" in
    *"gateways:"*"matrix"*) ok "matrix gateway planned";;
    *) fail "matrix gateway plan (got: $OUT)";;
esac

echo "== hermes-style self-improvement =="
rm -rf "$WS/skills/autoskill"
OUT=$(timeout 90 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "use-many-tools now" 2>/dev/null)
check "auto-learn extracted a skill" test -f "$WS/skills/autoskill/SKILL.md"
rm -rf "$WS/skills/autoskill"
OUT=$(CLAWC_AUTO_LEARN=0 timeout 90 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "use-many-tools now" 2>/dev/null)
check "auto-learn respects off switch" bash -c "test ! -f '$WS/skills/autoskill/SKILL.md'"
OUT=$(timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "use-search-history now" 2>/dev/null)
case "$OUT" in
    *jsonl:*hello\ telegram*) ok "search recalls session history";;
    *) fail "search history recall (got: $OUT)";;
esac
timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "use-write-skill now" >/dev/null 2>&1
OUT=$(timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "use-skill-nudge now" 2>/dev/null)
case "$OUT" in
    *"refine this skill with write_skill"*) ok "skill refinement nudge";;
    *) fail "skill refinement nudge (got: $OUT)";;
esac

echo "== /goal command =="
rm -f "$WS/GOALS.md"
rm -f "$WS/history"/tg-offset-*
CLAWC_TELEGRAM_API="$TAPI" timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        telegram -token GOALCMDTOKEN --once -poll 1 >/dev/null 2>&1
check "/goal add writes GOALS.md" grep -q "(g1) learn the drums" "$WS/GOALS.md"
rm -f "$REQDIR"/req-*.json
CLAWC_TELEGRAM_API="$TAPI" timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        telegram -token GOALCMDTOKEN --once -poll 1 >/dev/null 2>&1
check "/goal lists goals to chat" grep -q "learn the drums" "$REQDIR"/req-*.json
CLAWC_TELEGRAM_API="$TAPI" timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        telegram -token GOALCMDTOKEN --once -poll 1 >/dev/null 2>&1
check "/goal done completes goal" grep -q "\[x\] (g1) \[done" "$WS/GOALS.md"
CLAWC_TELEGRAM_API="$TAPI" timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        telegram -token GOALCMDTOKEN --once -poll 1 >/dev/null 2>&1
check "/goal rm removes goal" bash -c "! grep -q 'learn the drums' '$WS/GOALS.md'"

echo "== autonomous mode =="
rm -f "$WS/GOALS.md"
OUT=$(timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "use-goal-add now" 2>/dev/null)
check "goal added to GOALS.md" grep -q "(g1) learn the ukulele" "$WS/GOALS.md"
rm -f "$REQDIR"/req-*.json
timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "just do it" >/dev/null 2>&1
LAST=$(grep -l '"messages"' "$REQDIR"/req-*.json | sort -V | tail -1)
check "goals visible to model" grep -q "learn the ukulele" "$LAST"
OUT=$(timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "use-goal-done now" 2>/dev/null)
check "goal marked done" grep -q "\[x\] (g1)" "$WS/GOALS.md"
OUT=$(timeout 90 "$CLAWC" -q -w "$WS" -u "$API" -k testkey autonomous 2>/dev/null)
case "$OUT" in
    *FINAL*) ok "autonomous cycle reports progress";;
    *) fail "autonomous cycle (got: $OUT)";;
esac
check "autonomous advanced a goal" grep -q "made autonomous progress" "$WS/GOALS.md"
echo '{"autonomous_interval": 600}' > /tmp/clawc-au-cfg.json
check "autonomous gateway planned" bash -c ""$CLAWC" -c /tmp/clawc-au-cfg.json config | grep -q 'autonomous'"

echo "== self-improvement =="
rm -f "$WS/LESSONS.md"
rm -rf "$WS/skills/greet"
OUT=$(timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "use-reflect now" 2>/dev/null)
check "lesson recorded to LESSONS.md" grep -q "pizza protocol" "$WS/LESSONS.md"
rm -f "$REQDIR"/req-*.json
timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "just do it" >/dev/null 2>&1
LAST=$(grep -l '"messages"' "$REQDIR"/req-*.json | sort -V | tail -1)
check "lesson guides future prompts" grep -q "pizza protocol" "$LAST"
OUT=$(timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "use-write-skill now" 2>/dev/null)
check "skill written" test -f "$WS/skills/greet/SKILL.md"
rm -f "$REQDIR"/req-*.json
timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "just do it" >/dev/null 2>&1
LAST=$(grep -l '"messages"' "$REQDIR"/req-*.json | sort -V | tail -1)
check "new skill offered to model" grep -q "greet - greeting procedure" "$LAST"
OUT=$(timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "use-write-skill-bad now" 2>/dev/null)
check "skill name path tricks rejected" bash -c "test ! -f '$WS/evil' && test ! -e '$WS/skills/../evil'"
OUT=$(timeout 90 "$CLAWC" -q -w "$WS" -u "$API" -k testkey improve 2>/dev/null)
check "improve verb records lessons" grep -q "improve loop works" "$WS/LESSONS.md"

echo "== telegram media + vision =="
rm -f "$WS/history"/tg-offset-*
rm -rf "$WS/media"
rm -f "$REQDIR"/req-*.json
CLAWC_TELEGRAM_API="$TAPI" timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        telegram -token PHOTOTOKEN --once -poll 1 >/dev/null 2>&1
check "photo saved to workspace media" bash -c "ls $WS/media/ph-big.jpg >/dev/null 2>&1"
check "vision image in llm request" grep -q "data:image/jpeg;base64" "$REQDIR"/req-*.json
check "caption reached the agent" grep -q "describe this picture" "$REQDIR"/req-*.json

echo "== group mention gating =="
rm -f "$WS/history"/tg-offset-*
rm -f "$REQDIR"/req-*.json
CLAWC_TELEGRAM_API="$TAPI" timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        telegram -token GROUPTOKEN --once -poll 1 >/dev/null 2>&1
check "mentioned group msg answered" grep -q "hello group" "$REQDIR"/req-*.json
check "unmentioned group msg ignored" bash -c "! grep -q 'ignore me completely' '$REQDIR'/req-*.json"

echo "== build variants =="
rm -f /tmp/clawc-min
make -s CHANNELS=serve TARGET=/tmp/clawc-min >/dev/null 2>&1
check "minimal build compiles" test -x /tmp/clawc-min
make -s TLS=0 TARGET=/tmp/clawc-notls >/dev/null 2>&1
check "TLS=0 build compiles (curl fallback + ws stubs)" test -x /tmp/clawc-notls
OUT=$(/tmp/clawc-notls version 2>/dev/null)
case "$OUT" in
    clawc*) ok "TLS=0 build runs";;
    *) fail "TLS=0 build runs (got: $OUT)";;
esac
OUT=$(/tmp/clawc-min doctor 2>/dev/null | head -3)
case "$OUT" in
    *"[features]"*"serve"*"cron"*"heartbeat"*"[config]"*) ok "channels-only build keeps the core";;
    *) fail "minimal features line (got: $OUT)";;
esac
OUT=$(timeout 60 /tmp/clawc-min -q -w "$WS" -u "$API" -k testkey run "just do it" 2>/dev/null)
case "$OUT" in
    *hello-from-tool*) ok "minimal build runs the agent";;
    *) fail "minimal build run (got: $OUT)";;
esac
check "minimal build rejects telegram verb" bash -c "/tmp/clawc-min telegram >/dev/null 2>&1; test \$? -eq 2"
check "minimal build hides telegram from help" bash -c "! /tmp/clawc-min 2>&1 | grep -q 'Telegram bot'"

echo "== doctor =="
OUT=$(timeout 60 "$CLAWC" -q -w "$WS" -u "$API" -k testkey doctor 2>/dev/null)
case "$OUT" in
    *"[llm]       ok"*"doctor: all good."*) ok "doctor reports healthy";;
    *) fail "doctor healthy (got: $OUT)";;
esac
OUT=$(timeout 60 "$CLAWC" -q -w /tmp/clawc-doctor-none -u "$API" -k testkey doctor 2>/dev/null)
case "$OUT" in
    *"[workspace] FAIL"*) ok "doctor flags problems";;
    *) fail "doctor flags problems (got: $OUT)";;
esac

echo "== subagent tool =="
rm -f "$REQDIR"/req-*.json
OUT=$(timeout 90 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "please use-a-subagent now" 2>/dev/null)
case "$OUT" in
    *pong-via-sub*) ok "subagent ran and reported back";;
    *) fail "subagent ran (got: $OUT)";;
esac
LAST=$(grep -l '"messages"' "$REQDIR"/req-*.json | sort -V | tail -1)
check "subagent tool chatter stayed out of parent" bash -c "! grep -q '\"echo pong-via-sub\"' '$LAST'"
OUT=$(timeout 90 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "please use-nested-subagent now" 2>/dev/null)
case "$OUT" in
    *"cannot nest"*) ok "subagent nesting refused";;
    *) fail "subagent nesting refused (got: $OUT)";;
esac
check "nesting refusal in audit" grep -q "cannot nest" "$WS/history/audit.jsonl"
OUT=$(timeout 90 "$CLAWC" -q -w "$WS" -u "$API" -k testkey run "please use-par-subagents now" 2>/dev/null)
CNT=$(echo "$OUT" | grep -o "pong-via-sub" | wc -l)
if [ "$CNT" -ge 2 ]; then ok "parallel subagents all reported"; else fail "parallel subagents (got: $OUT)"; fi
case "$OUT" in *"subagent report [1/2]"*) ok "parallel reports labelled";; *) fail "parallel report labels (got: $OUT)";; esac

echo "== cron 64-bit timestamp =="
YWS=/tmp/clawc-y2038-test
rm -rf "$YWS"; mkdir -p "$YWS"
"$CLAWC" -q -w "$YWS" cron add future --at 2208988800 >/dev/null 2>&1
check "cron persists timestamp beyond 2038" grep -q '2208988800' "$YWS/cron.json"

echo "== cron jobs =="
CWS=/tmp/clawc-test-cron-ws
rm -rf "$CWS" && mkdir -p "$CWS"
OUT=$("$CLAWC" -q -w "$CWS" -u "$API" -k testkey cron add "say the word mango" --at 1000000 2>/dev/null)
case "$OUT" in *added\ j1*) ok "cron add";; *) fail "cron add (got: $OUT)";; esac
OUT=$("$CLAWC" -q -w "$CWS" cron list 2>/dev/null)
case "$OUT" in *j1*one-shot*) ok "cron list shows job";; *) fail "cron list (got: $OUT)";; esac
OUT=$("$CLAWC" -w "$CWS" config)
case "$OUT" in *"gateways: cron"*) ok "cron gateway planned when schedule active";; *) fail "cron gateway plan (got: $OUT)";; esac
OUT=$(timeout 90 "$CLAWC" -q -w "$CWS" -u "$API" -k testkey cron --run-due 2>/dev/null)
case "$OUT" in *mango-delivered*) ok "due job fired";; *) fail "due job fired (got: $OUT)";; esac
OUT=$("$CLAWC" -q -w "$CWS" cron list 2>/dev/null)
case "$OUT" in *spent*) ok "one-shot job spent after firing";; *) fail "one-shot spent (got: $OUT)";; esac
OUT=$("$CLAWC" -q -w "$CWS" -u "$API" -k testkey cron add "say the word mango" --at 1000000 --every 3600 2>/dev/null)
case "$OUT" in *added\ j2*) ok "cron add recurring";; *) fail "cron add recurring (got: $OUT)";; esac
OUT=$(timeout 90 "$CLAWC" -q -w "$CWS" -u "$API" -k testkey cron --run-due 2>/dev/null)
case "$OUT" in *mango-delivered*) ok "recurring job fired";; *) fail "recurring fired (got: $OUT)";; esac
OUT=$("$CLAWC" -q -w "$CWS" cron list 2>/dev/null)
case "$OUT" in *j2*recurring*) ok "recurring job re-armed";; *) fail "recurring re-armed (got: $OUT)";; esac
"$CLAWC" -q -w "$CWS" cron rm j2 >/dev/null 2>&1
OUT=$("$CLAWC" -q -w "$CWS" cron list 2>/dev/null)
case "$OUT" in *j2*) fail "cron rm (still listed)";; *) ok "cron rm";; esac
"$CLAWC" -q -w "$CWS" cron add "time test daily" --daily 09:00 >/dev/null 2>&1
check "daily time spec sets 86400s anchor" grep -q '"task": "time test daily", "at": [0-9]*, "every": 86400' "$CWS/cron.json"
"$CLAWC" -q -w "$CWS" cron add "time test weekly" --weekly mon 09:00 >/dev/null 2>&1
check "weekly time spec sets 7d anchor" grep -q '"task": "time test weekly", "at": [0-9]*, "every": 604800' "$CWS/cron.json"
"$CLAWC" -q -w "$CWS" cron add "time test once" --at 23:59 >/dev/null 2>&1
check "clock one-shot stored" grep -q '"task": "time test once", "at": [0-9]*, "every": 0' "$CWS/cron.json"
check "bad time spec rejected" bash -c ""$CLAWC" -q -w '$CWS' cron add x --daily 99:99 >/dev/null 2>&1; test \$? -eq 2"


echo "== JSON empty-object parsing =="
"$CLAWC" -q -w "$WS" -u "$API" -k testkey serve -p 8878 &
SERVE_PID=$!
sleep 0.5
REPLY=$(python3 - <<'EOF'
import json, os, socket
s = socket.create_connection(("127.0.0.1", 8878), timeout=30)
s.sendall(json.dumps({"text":"ping via bridge", "pad":{}, "nested":{"a":{},"b":{}}, "token":os.environ["CLAWC_BRIDGE_TOKEN"]}).encode()+b"\n")
buf = b""
while not buf.endswith(b"\n"):
    d = s.recv(4096)
    if not d: break
    buf += d
print(buf.decode())
EOF
)
kill $SERVE_PID 2>/dev/null
case "$REPLY" in
    *'"ok":true'*) ok "empty JSON objects parse";;
    *) fail "empty JSON objects (got: $REPLY)";;
esac

echo "== list_dir defaults to the workspace =="
OUT=$("$CLAWC" -q -w "$WS" -u "$API" -k testkey run "use-listdir now" 2>/dev/null)
case "$OUT" in
    *"SOUL.md"*) ok "list_dir lists workspace without a path";;
    *) fail "list_dir workspace default (got: $OUT)";;
esac

echo "== config precedence (file < env < flags) =="
CFGD=/tmp/clawc-test-cfg
mkdir -p "$CFGD"
cat > "$CFGD/base.json" <<JEOF
{
  "_note": "test config for clawc daemon",
  "workspace": "$WS",
  "model": "filemodel",
  "api_url": "$API",
  "api_key": "filekey",
  "serve_port": 8879,
  "heartbeat_interval": 2,
  "auto_approve": true,
  "telegram_token": "DAEMONTOKEN",
  "telegram_api": "$TAPI",
  "telegram_poll": 1,
  "telegram_allow": "42",
  "timeout": 30
}
JEOF
cat > "$CFGD/legacy.conf" <<LEOF
# legacy format - must be rejected
workspace = $WS
model = legacymodel
api_url = $API
api_key = k
LEOF
OUT=$("$CLAWC" -c "$CFGD/base.json" config)
case "$OUT" in
    *"model: filemodel (file)"*) ok "JSON config values loaded";;
    *) fail "JSON config values loaded (got: $OUT)";;
esac
BAK="$OUT"
case "$BAK" in
    *"gateways: telegram serve heartbeat"*) ok "daemon gateway plan from config";;
    *) fail "daemon gateway plan (got: $BAK)";;
esac
case "$BAK" in
    *"auto_approve: on (file)"*) ok "JSON boolean value mapped";;
    *) fail "JSON boolean value mapped (got: $BAK)";;
esac
check "legacy key=value format now rejected" bash -c ""$CLAWC" -c $CFGD/legacy.conf config >/dev/null 2>&1; test \$? -eq 2"
OUT=$(CLAWC_MODEL=envmodel "$CLAWC" -c "$CFGD/base.json" config)
case "$OUT" in
    *"model: envmodel (env)"*) ok "env overrides config file";;
    *) fail "env overrides config file (got: $OUT)";;
esac
OUT=$(CLAWC_MODEL=envmodel "$CLAWC" -c "$CFGD/base.json" -m flagmodel config)
case "$OUT" in
    *"model: flagmodel (flag)"*) ok "flag overrides env and file";;
    *) fail "flag overrides env (got: $OUT)";;
esac
OUT=$(CLAWC_CONFIG="$CFGD/base.json" "$CLAWC" config)
case "$OUT" in
    *"model: filemodel (file)"*) ok "CLAWC_CONFIG env selects file";;
    *) fail "CLAWC_CONFIG env (got: $OUT)";;
esac
echo '{"serve_bind": "0.0.0.0", "context_chars": 400}' > "$CFGD/bind.json"
check "serve_bind config key loads" bash -c ""$CLAWC" -c $CFGD/bind.json config | grep -q 'serve_bind: 0.0.0.0 (file)'"
check "context_chars config key loads" bash -c ""$CLAWC" -c $CFGD/bind.json config | grep -q 'context_chars: 400 (file)'"
check "missing config file errors" bash -c ""$CLAWC" -c $CFGD/nope.conf config >/dev/null 2>&1; test \$? -eq 2"
echo '{"bogus_key": 1}' > "$CFGD/bad.conf"
check "unknown config key errors" bash -c ""$CLAWC" -c $CFGD/bad.conf config >/dev/null 2>&1; test \$? -eq 2"
check "daemon with no gateways errors" bash -c ""$CLAWC" daemon >/dev/null 2>&1; test \$? -eq 2"

echo "== setup wizard =="
WIZ=$(printf '%s\n' "$WS" "$TAPI" "wizardmodel" "wizkey" "TESTTOKEN" "42" "42" "0" "0" "no" \
      | CLAWC_TELEGRAM_API="$TAPI" CLAWC_CONFIG="$CFGD/deep/nest/wizard.conf" "$CLAWC" setup 2>&1)
check "wizard wrote config (created its directory)" test -s "$CFGD/deep/nest/wizard.conf"
check "wizard wrote sender IDs" grep -q '"telegram_users": "42"' "$CFGD/deep/nest/wizard.conf"
check "wizard config is JSON" grep -q '^{' "$CFGD/deep/nest/wizard.conf"
check "wizard appended /v1 to bare base url" grep -q "\"api_url\": \"$API\"" "$CFGD/deep/nest/wizard.conf"
echo "$WIZ" | grep -q "verified: @mock_bot" && ok "wizard verified the bot token" \
    || fail "wizard token verification (got: $WIZ)"
check "wizard config is private" bash -c "test \"\$(stat -c %a '$CFGD/deep/nest/wizard.conf')\" = 600"
OUT=$("$CLAWC" -c "$CFGD/deep/nest/wizard.conf" config)
case "$OUT" in
    *"model: wizardmodel (file)"*) ok "wizard config loads";;
    *) fail "wizard config loads (got: $OUT)";;
esac
case "$OUT" in
    *"gateways: telegram"*) ok "wizard gateway plan includes telegram";;
    *) fail "wizard gateway plan (got: $OUT)";;
esac
WIZ=$(printf '%s\n' "$WS" "$API" "wizardmodel" "wizkey" "BADTOKEN" "" "42" "0" "0" "no" \
      | CLAWC_TELEGRAM_API="$TAPI" CLAWC_CONFIG="$CFGD/wizardbad.conf" "$CLAWC" setup 2>&1)
if [ $? -ne 0 ]; then ok "wizard rejects bad token"; else fail "wizard rejects bad token"; fi
check "bad wizard wrote nothing" bash -c "test ! -f '$CFGD/wizardbad.conf'"

echo "== daemon supervisor =="
printf '# Heartbeat checklist\n\n- [ ] idle\n' > "$WS/HEARTBEAT.md"
rm -f "$REQDIR"/req-*.json
"$CLAWC" -q daemon -c "$CFGD/base.json" >/tmp/clawc-test-daemon.log 2>&1 &
DAEMON_PID=$!
sleep 3
check "daemon supervises 3 gateways" grep -q "supervising 3 gateway(s)" /tmp/clawc-test-daemon.log
check "telegram gateway started" grep -q "gateway telegram: started" /tmp/clawc-test-daemon.log
check "serve gateway started" grep -q "gateway serve: started" /tmp/clawc-test-daemon.log
check "heartbeat gateway started" grep -q "gateway heartbeat: started" /tmp/clawc-test-daemon.log
check "telegram gateway processed a message" grep -q '"chat_id":42' "$REQDIR"/req-*.json
check "heartbeat gateway cycled" grep -q "HEARTBEAT_OK" /tmp/clawc-test-daemon.log
REPLY=$(python3 - <<'EOF'
import json, os, socket
s = socket.create_connection(("127.0.0.1", 8879), timeout=30)
s.sendall(json.dumps({"text":"ping via bridge", "token":os.environ["CLAWC_BRIDGE_TOKEN"]}).encode()+b"\n")
buf = b""
while not buf.endswith(b"\n"):
    d = s.recv(4096)
    if not d: break
    buf += d
print(buf.decode())
EOF
)
case "$REPLY" in
    *'"ok":true'*) ok "serve gateway answers via daemon";;
    *) fail "serve gateway via daemon (got: $REPLY)";;
esac
CHILD=$(pgrep -P $DAEMON_PID | head -1)
kill -9 "$CHILD" 2>/dev/null
sleep 3.5
check "crashed gateway logged" grep -q "restart in" /tmp/clawc-test-daemon.log
NCH=$(pgrep -P $DAEMON_PID | wc -l)
if [ "$NCH" -eq 3 ]; then ok "gateways restored after crash"; else fail "gateways after crash: $NCH"; fi
echo "- [ ] run the command rm -rf /tmp/clawc-risky-test" > "$WS/HEARTBEAT.md"
sleep 5
check "heartbeat notify forwarded to telegram" grep -q '"chat_id":42,"text":"FINAL: refused' "$REQDIR"/req-*.json
kill -TERM $DAEMON_PID 2>/dev/null
sleep 2
check "daemon shuts down cleanly" grep -q "\[daemon\] stopped" /tmp/clawc-test-daemon.log
check "daemon children reaped" bash -c "! pgrep -P $DAEMON_PID"

echo "== sender authorization and requester-bound confirmations =="
# Missing user allowlists must stop each gateway and the daemon, before network I/O.
for channel in telegram matrix discord; do
    case "$channel" in
        telegram) envname=CLAWC_TELEGRAM_USERS; export CLAWC_TELEGRAM_TOKEN=TGAUTH CLAWC_TELEGRAM_API="$TAPI" ;;
        matrix) envname=CLAWC_MATRIX_USERS; export CLAWC_MATRIX_HOMESERVER="$TAPI" CLAWC_MATRIX_TOKEN=MXAUTH ;;
        discord) envname=CLAWC_DISCORD_USERS; export CLAWC_DISCORD_TOKEN=DCAUTH CLAWC_DISCORD_GATEWAY="ws://127.0.0.1:$((PORT+100))" CLAWC_DISCORD_API="$TAPI" ;;
    esac
    env -u "$envname" "$CLAWC" -q -w "$WS" -u "$API" -k testkey "$channel" >/tmp/clawc-auth-missing.log 2>&1
    rc=$?
    if [ "$rc" -eq 2 ] && grep -q "${channel}_users required" /tmp/clawc-auth-missing.log; then
        ok "$channel requires configured sender IDs"
    else fail "$channel missing sender allowlist (exit $rc)"; fi
done
unset CLAWC_MATRIX_TOKEN CLAWC_MATRIX_HOMESERVER CLAWC_DISCORD_TOKEN CLAWC_DISCORD_GATEWAY CLAWC_DISCORD_API
if env -u CLAWC_TELEGRAM_USERS "$CLAWC" -q -w "$WS" -u "$API" -k testkey daemon >/tmp/clawc-auth-daemon.log 2>&1; then
    fail "daemon rejects missing sender list"
elif grep -q 'telegram_users required' /tmp/clawc-auth-daemon.log; then ok "daemon rejects missing sender list without restart loop"
else fail "daemon missing sender diagnostic"; fi
unset CLAWC_TELEGRAM_TOKEN CLAWC_TELEGRAM_API

# JSON config and separator-only lists have the same fail-closed semantics.
cat > "$WS/sender-config.json" <<'EOF'
{"telegram_users":"777,778","matrix_users":"@alice:localhost,@bob:localhost","discord_users":"u1,u2"}
EOF
OUT=$(env -u CLAWC_TELEGRAM_USERS -u CLAWC_MATRIX_USERS -u CLAWC_DISCORD_USERS "$CLAWC" -c "$WS/sender-config.json" config)
case "$OUT" in
    *"telegram_users: 777,778 (file)"*"matrix_users: @alice:localhost,@bob:localhost (file)"*"discord_users: u1,u2 (file)"*) ok "sender IDs load from JSON";;
    *) fail "sender IDs load from JSON";;
esac
printf '{"discord_users":123456789012345678}\n' > "$WS/sender-bad.json"
if env -u CLAWC_DISCORD_USERS "$CLAWC" -c "$WS/sender-bad.json" config >/tmp/clawc-auth-bad.log 2>&1; then
    fail "numeric JSON sender IDs rejected"
elif grep -q 'discord_users must be a string' /tmp/clawc-auth-bad.log; then ok "numeric JSON sender IDs rejected without rounding"
else fail "numeric sender ID diagnostic"; fi
if CLAWC_TELEGRAM_USERS=' , , ' CLAWC_TELEGRAM_TOKEN=TGAUTH "$CLAWC" -q -w "$WS" -u "$API" -k testkey telegram >/tmp/clawc-auth-empty.log 2>&1; then
    fail "separator-only sender list rejected"
elif grep -q 'telegram_users required' /tmp/clawc-auth-empty.log; then ok "separator-only sender list rejected"
else fail "separator-only sender diagnostic"; fi

# Unknown or missing Telegram senders cannot invoke commands or download media.
rm -f "$WS/history"/tg-offset-* "$REQDIR"/req-*.json
CLAWC_TELEGRAM_API="$TAPI" timeout 15 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
    telegram -token TGAUTH -allow 777 --once -poll 1 >/dev/null 2>&1
check "telegram authorized user reaches help" grep -q "clawc agent online" "$REQDIR"/req-*.json
check "telegram unauthorized attachment not fetched" bash -c "! grep -q 'unauthorized-image' '$REQDIR'/req-*.json"
check "telegram allowed user in disallowed chat cannot download" bash -c "! grep -q 'wrong-chat-image' '$REQDIR'/req-*.json"
check "telegram unauthorized goal not executed" bash -c "! grep -q 'attacker goal' '$WS/GOALS.md'"
check "telegram missing sender not sent to model" bash -c "! grep -q 'missing sender attack' '$REQDIR'/req-*.json"
for tok in TGOTHERBTN TGOTHERTEXT TGSTALETEXT; do
    rm -rf /tmp/clawc-risky-test; mkdir -p /tmp/clawc-risky-test; touch /tmp/clawc-risky-test/marker
    rm -f "$WS/history"/tg-offset-* "$REQDIR"/req-*.json
    CLAWC_TELEGRAM_API="$TAPI" CLAWC_CONFIRM_SECS=6 timeout 15 "$CLAWC" -q -w "$WS" -u "$API" -k testkey \
        telegram -token "$tok" --once -poll 1 >/dev/null 2>&1
    check "telegram $tok cannot approve" test -f /tmp/clawc-risky-test/marker
    check "telegram $tok legitimate decline observed" grep -q 'cancelled' "$REQDIR"/req-*.json
done
rm -rf /tmp/clawc-risky-test

# Matrix reply relation binds /yes and /no to the precise prompt event.
rm -f "$WS/history"/matrix-sync-* "$REQDIR"/req-*.json
CLAWC_MATRIX_HOMESERVER="$TAPI" CLAWC_MATRIX_TOKEN=MXAUTH timeout 15 "$CLAWC" -q -w "$WS" -u "$API" -k testkey matrix >/dev/null 2>&1
check "matrix authorized sender accepted" grep -q 'hello matrix' "$REQDIR"/req-*.json
check "matrix unauthorized and missing senders ignored" bash -c "! grep -qE 'attacker matrix|missing matrix' '$REQDIR'/req-*.json"
for tok in MXOTHER MXSTALE MXYES; do
    rm -rf /tmp/clawc-risky-test; mkdir -p /tmp/clawc-risky-test; touch /tmp/clawc-risky-test/marker
    rm -f "$WS/history"/matrix-sync-* "$REQDIR"/req-*.json
    CLAWC_MATRIX_HOMESERVER="$TAPI" CLAWC_MATRIX_TOKEN="$tok" CLAWC_CONFIRM_SECS=6 timeout 15 \
        "$CLAWC" -q -w "$WS" -u "$API" -k testkey matrix >/dev/null 2>&1
    if [ "$tok" = MXYES ]; then
        check "matrix requester reply approves" bash -c 'test ! -f /tmp/clawc-risky-test/marker'
    else
        check "matrix $tok cannot approve" test -f /tmp/clawc-risky-test/marker
        check "matrix $tok legitimate decline observed" grep -q 'cancelled' "$REQDIR"/req-*.json
    fi
done
rm -rf /tmp/clawc-risky-test

# Discord verifies author.id or guild member.user.id / DM user.id before dispatch.
rm -f "$WS/history"/discord-cursor-* "$REQDIR"/req-*.json "$WS/media/evil.png"
CLAWC_DISCORD_TOKEN=DCAUTH CLAWC_DISCORD_GATEWAY="ws://127.0.0.1:$((PORT+100))" \
    CLAWC_DISCORD_API="$TAPI" timeout 15 "$CLAWC" -q -w "$WS" -u "$API" -k testkey discord >/dev/null 2>&1
check "discord authorized user reaches help" grep -q 'clawc online' "$REQDIR"/req-*.json
check "discord unauthorized attachment not downloaded" bash -c "test ! -f '$WS/media/evil.png'"
check "discord unauthorized slash not dispatched" bash -c "! grep -q 'attacker slash' '$REQDIR'/req-*.json"
check "discord unauthorized or missing user goal not executed" bash -c "! grep -qE 'attacker goal|missing goal|spoofed guild goal' '$WS/GOALS.md'"
rm -f "$WS/history"/discord-cursor-* "$REQDIR"/req-*.json
CLAWC_DISCORD_TOKEN=DCGUILD CLAWC_DISCORD_GATEWAY="ws://127.0.0.1:$((PORT+100))" \
    CLAWC_DISCORD_API="$TAPI" timeout 15 "$CLAWC" -q -w "$WS" -u "$API" -k testkey discord >/dev/null 2>&1
check "discord guild member.user identity accepted" grep -q 'mango-delivered' "$REQDIR"/req-*.json
for tok in DCOTHERBTN DCOTHERTEXT DCSTALETEXT DCTEXTYES; do
    rm -rf /tmp/clawc-risky-test; mkdir -p /tmp/clawc-risky-test; touch /tmp/clawc-risky-test/marker
    rm -f "$WS/history"/discord-cursor-* "$REQDIR"/req-*.json
    CLAWC_DISCORD_TOKEN="$tok" CLAWC_DISCORD_GATEWAY="ws://127.0.0.1:$((PORT+100))" \
        CLAWC_DISCORD_API="$TAPI" CLAWC_CONFIRM_SECS=6 timeout 15 \
        "$CLAWC" -q -w "$WS" -u "$API" -k testkey discord >/dev/null 2>&1
    if [ "$tok" = DCTEXTYES ]; then
        check "discord requester post-prompt text approves" bash -c 'test ! -f /tmp/clawc-risky-test/marker'
    else
        check "discord $tok cannot approve" test -f /tmp/clawc-risky-test/marker
        check "discord $tok legitimate decline observed" grep -q 'cancelled' "$REQDIR"/req-*.json
    fi
done
rm -rf /tmp/clawc-risky-test

echo
echo "passed: $PASS  failed: $FAIL"
[ "$FAIL" -eq 0 ]
