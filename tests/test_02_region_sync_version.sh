#!/usr/bin/env bash
set -euo pipefail

CENTRAL_PORT=6000
TLS_PORT=5555
UDP_PORT=7777
SYNC_INTERVAL_S=1
COMM_LOST_TIMEOUT_S=20

CERT=cert.pem
KEY=key.pem

cleanup(){
  set +e
  [[ -n "${REGION_PID:-}" ]] && kill "$REGION_PID" 2>/dev/null || true
  [[ -n "${CENTRAL_PID:-}" ]] && kill "$CENTRAL_PID" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

wait_for_port(){
  local port="$1"
  for i in {1..20}; do
    if nc -z 127.0.0.1 "$port" 2>/dev/null; then
      return 0
    fi
    sleep 0.3
  done
  return 1
}

extract_region1_version(){
  # Prima snapshot kao string argument
  python3 - "$1" <<'PY'
import re, sys
txt = sys.argv[1]
m = re.search(r"Region\s+1\s*\(version\s+(\d+)\)", txt)
print(m.group(1) if m else "-1")
PY
}

echo "[info] starting central_server..."
./central_server "$CENTRAL_PORT" "$CERT" "$KEY" > tests/_central.log 2>&1 &
CENTRAL_PID=$!
sleep 1

wait_for_port "$CENTRAL_PORT" || {
  echo "[FAIL] central did not open port"
  tail -n 50 tests/_central.log || true
  exit 1
}

echo "[info] central up"

echo "[info] starting regional_server..."
./regional_server 1 "$TLS_PORT" "$UDP_PORT" 127.0.0.1 "$CENTRAL_PORT" "$SYNC_INTERVAL_S" "$CERT" "$KEY" "$COMM_LOST_TIMEOUT_S" \
  > tests/_regional.log 2>&1 &
REGION_PID=$!
sleep 1

wait_for_port "$TLS_PORT" || {
  echo "[FAIL] regional did not open TLS port"
  tail -n 50 tests/_regional.log || true
  exit 1
}

echo "[info] regional up"

echo "[test_02] taking initial snapshot..."
out0="$(./admin_cli 127.0.0.1 "$CENTRAL_PORT")"
echo "$out0"

v0="$(extract_region1_version "$out0")"

if [[ "$v0" -lt 0 ]]; then
  echo "[FAIL] Could not find Region 1 version in snapshot"
  exit 1
fi

echo "[info] initial version=$v0"
echo "[test_02] waiting for version to increase..."

success=0
for i in {1..10}; do
  sleep 1
  out="$(./admin_cli 127.0.0.1 "$CENTRAL_PORT")"
  echo "$out"
  v="$(extract_region1_version "$out")"

  if [[ "$v" -gt "$v0" ]]; then
    echo "[OK] version increased: $v0 -> $v"
    success=1
    break
  fi
done

if [[ "$success" -ne 1 ]]; then
  echo "[debug] regional log:"
  tail -n 100 tests/_regional.log || true
  echo "[debug] central log:"
  tail -n 100 tests/_central.log || true
  echo "[FAIL] Region version did not increase"
  exit 1
fi

echo "[PASS] test_02_region_sync_version completed."
