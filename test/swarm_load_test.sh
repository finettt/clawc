#!/usr/bin/env bash
# Prototype load test: distinct agents share a fixed worker pool.
set -euo pipefail
cd "$(dirname "$0")/.."
CLAWC="${CLAWC:-build/native/clawc}"   # produced by `make`
D=$(mktemp -d /tmp/clawc-swarm-load.XXXXXX); MP=''
cleanup() { [ -z "$MP" ] || kill "$MP" 2>/dev/null || :; rm -rf "$D"; rm -f /tmp/swarm-peak; }
trap cleanup EXIT
make -s
python3 test/swarm_parallel_mock.py >"$D/mock.log" 2>&1 & MP=$!
sleep .3; kill -0 "$MP"
python3 - "$D/spec.json" <<'PY'
import json, sys
n=100
s={'workers':8,'agents':[{'id':f'a{i}','role':'test'} for i in range(n)],
   'messages':[{'to':f'@a{i}','text':'test'} for i in range(n)]}
open(sys.argv[1],'w').write(json.dumps(s))
PY
"$CLAWC" -w "$D/workspace" init >/dev/null
"$CLAWC" -w "$D/workspace" -u http://127.0.0.1:19998/v1 -k test swarm "$D/spec.json" >"$D/out" 2>"$D/err"
[ "$(grep -c '"report":"ok"' "$D/out")" -eq 100 ]
peak=$(cat /tmp/swarm-peak)
[ "$peak" -ge 2 ] && [ "$peak" -le 8 ]
printf 'ok: 100 agents completed, peak concurrent model requests %s/8\n' "$peak"
