#!/usr/bin/env bash
set -euo pipefail
source tests/common.sh

[[ -f "$CERT" ]] || die "Missing cert: $CERT"
[[ -f "$KEY"  ]] || die "Missing key: $KEY"

# (da ne bude bind error)
pkill -f central_server || true
sleep 1

echo "[info] starting central_server..."
./central_server "$CENTRAL_PORT" "$CERT" "$KEY" > tests/_central.log 2>&1 &
CENTRAL_PID=$!
trap 'cleanup_pids "$CENTRAL_PID"' EXIT INT TERM

sleep 1
require_running "$CENTRAL_PID" "central_server" "tests/_central.log"
wait_for_port "$CENTRAL_PORT" 10 || die "central not listening on $CENTRAL_PORT"

echo "[info] running admin_cli snapshot..."
out="$(snapshot)"
echo "$out"

echo "[PASS] central snapshot smoke OK"
