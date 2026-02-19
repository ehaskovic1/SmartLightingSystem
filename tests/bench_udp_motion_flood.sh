#!/usr/bin/env bash
set -euo pipefail
source tests/common.sh

COUNT="${COUNT:-1000}"
URI="${SENSOR_URI:-sls://sensor-001}"
ZONE="${ZONE:-1}"

pkill -f central_server || true
pkill -f regional_server || true
sleep 1

echo "[info] start central..."
./central_server "$CENTRAL_PORT" "$CERT" "$KEY" > tests/_central.log 2>&1 &
CENTRAL_PID=$!
sleep 1
require_running "$CENTRAL_PID" "central_server" "tests/_central.log"
wait_for_port "$CENTRAL_PORT" 10 || die "central not listening"

echo "[info] start regional..."
./regional_server 1 "$TLS_PORT" "$UDP_PORT" 127.0.0.1 "$CENTRAL_PORT" 1 "$CERT" "$KEY" "$COMM_LOST_TIMEOUT_S" \
  > tests/_regional.log 2>&1 &
REGION_PID=$!
sleep 1
require_running "$REGION_PID" "regional_server" "tests/_regional.log"
wait_for_port "$TLS_PORT" 10 || die "regional TLS not listening"

trap 'cleanup_pids "$REGION_PID" "$CENTRAL_PID"' EXIT INT TERM

build_udp_sender

echo "[info] sending $COUNT UDP telemetry packets..."
t0="$(now_ms)"
for i in $(seq 1 "$COUNT"); do
  m=$(( i % 2 ))
  tests/_tmp/udp_send_bin 127.0.0.1 "$UDP_PORT" "$URI" "$ZONE" "$m" >/dev/null
done
t1="$(now_ms)"

echo "[RESULT] sent=$COUNT elapsed_ms=$((t1-t0)) rate_pkt_s=$(( (COUNT*1000) / (t1-t0+1) ))"