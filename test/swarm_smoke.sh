#!/usr/bin/env bash
# Prototype smoke test; does not assert the production binary-size contracts.
set -euo pipefail
cd "$(dirname "$0")/.."
CLAWC="${CLAWC:-build/native/clawc}"   # produced by `make`
D=$(mktemp -d /tmp/clawc-swarm.XXXXXX)
MP='' SP=''
cleanup() { [ -z "$MP" ] || kill "$MP" 2>/dev/null || :; [ -z "$SP" ] || kill "$SP" 2>/dev/null || :; rm -rf "$D"; }
trap cleanup EXIT
make -s
make -s CHANNELS=telegram TARGET="$D/clawc-budget"
python3 test/swarm_mock.py >"$D/mock.log" 2>&1 & MP=$!
sleep .3
kill -0 "$MP"
"$CLAWC" -w "$D/local" init >/dev/null
"$CLAWC" -w "$D/remote" init >/dev/null
cat > "$D/spec.json" <<'JSON'
{"workers":2,"agents":[{"id":"alice","role":"planner"},{"id":"bob","role":"reviewer","peer":"buddy"}],"groups":[{"id":"team","members":["alice","bob"]}],"messages":[{"to":"@alice","text":"start"}]}
JSON
export CLAWC_BRIDGE_TOKEN=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
export CLAWC_SERVE_TLS=0 CLAWC_A2A_TLS=0 CLAWC_A2A_PEERS='buddy=127.0.0.1:19997'
"$CLAWC" -w "$D/remote" -u http://127.0.0.1:19998/v1 -k test serve -p 19997 >"$D/serve.log" 2>&1 & SP=$!
sleep .3
kill -0 "$SP"
for cmd in "$CLAWC" "$D/clawc-budget"; do
    rm -f "$D/local/history/swarm-"*.jsonl "$D/remote/history/srv-"*.jsonl
    "$cmd" -w "$D/local" -u http://127.0.0.1:19998/v1 -k test swarm "$D/spec.json" >"$D/out" 2>"$D/err"
    grep -q '"from":"alice","to":"@bob"' "$D/out"
    grep -q '"from":"bob","to":"#team"' "$D/out"
    grep -q '"report":"alice received group"' "$D/out"
    echo "ok: $cmd direct, group and remote routing"
done
