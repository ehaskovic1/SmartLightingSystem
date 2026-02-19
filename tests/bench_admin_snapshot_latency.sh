
#!/usr/bin/env bash
set -euo pipefail
source tests/common.sh

N="${N:-50}"

pkill -f central_server || true
sleep 1

./central_server "$CENTRAL_PORT" "$CERT" "$KEY" > tests/_central.log 2>&1 &
CENTRAL_PID=$!
trap 'cleanup_pids "$CENTRAL_PID"' EXIT INT TERM

sleep 1
require_running "$CENTRAL_PID" "central_server" "tests/_central.log"
wait_for_port "$CENTRAL_PORT" 10 || die "central not listening"

echo "[info] running $N snapshots..."
python3 - "$N" "$CENTRAL_PORT" <<'PY'
import subprocess, time, sys, statistics
N=int(sys.argv[1]); port=sys.argv[2]
vals=[]
for i in range(N):
  t0=time.time()
  subprocess.check_call(["./admin_cli","127.0.0.1",port], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
  vals.append((time.time()-t0)*1000.0)

vals_sorted=sorted(vals)
def pct(p):
  k=int(round((p/100.0)*(len(vals_sorted)-1)))
  return vals_sorted[k]
print(f"count={N}")
print(f"avg_ms={statistics.mean(vals):.2f}")
print(f"p50_ms={pct(50):.2f}")
print(f"p90_ms={pct(90):.2f}")
print(f"p95_ms={pct(95):.2f}")
print(f"max_ms={max(vals):.2f}")
PY