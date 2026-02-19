#!/usr/bin/env bash
set -euo pipefail

CENTRAL_PORT="${CENTRAL_PORT:-6000}"
TLS_PORT="${TLS_PORT:-5555}"
UDP_PORT="${UDP_PORT:-7777}"
SYNC_INTERVAL_S="${SYNC_INTERVAL_S:-1}"
COMM_LOST_TIMEOUT_S="${COMM_LOST_TIMEOUT_S:-20}"

CERT="${CERT:-cert.pem}"
KEY="${KEY:-key.pem}"

ZONE="${ZONE:-1}"
LAMP_URI="${LAMP_URI:-sls://lamp-001}"
SENSOR_URI="${SENSOR_URI:-sls://sensor-001}"

WAIT_MAX_S="${WAIT_MAX_S:-20}"
POLL_S="${POLL_S:-1}"

mkdir -p tests

die(){ echo "[FAIL] $*" >&2; exit 1; }

cleanup(){
  set +e
  echo "[cleanup] stopping processes..."
  for pid in "${SENSOR_PID:-}" "${LAMP_PID:-}" "${REGION_PID:-}" "${CENTRAL_PID:-}"; do
    [[ -n "${pid}" ]] && kill -TERM "$pid" 2>/dev/null || true
  done
  sleep 0.5
  for pid in "${SENSOR_PID:-}" "${LAMP_PID:-}" "${REGION_PID:-}" "${CENTRAL_PID:-}"; do
    [[ -n "${pid}" ]] && kill -KILL "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

wait_listen(){
  local port="$1"
  local max_s="$2"
  local t=0
  while [[ "$t" -lt "$max_s" ]]; do
    if ss -ltn 2>/dev/null | awk '{print $4}' | grep -qE ":${port}\$"; then
      return 0
    fi
    sleep 0.2
    t=$((t+1))
  done
  return 1
}

require_running(){
  local pid="$1"
  local name="$2"
  local log="$3"
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "[debug] ${name} exited early. Tail of ${log}:"
    tail -n 120 "$log" 2>/dev/null || true
    die "${name} not running"
  fi
}

snapshot(){
  ./admin_cli 127.0.0.1 "$CENTRAL_PORT"
}

wait_alarm_uri(){
  local uri="$1"
  local label="$2"
  local max_s="$3"
  local poll_s="$4"

  echo "[test] waiting alarm for ${label} uri=${uri} ..."
  local t=0 out=""
  while [[ "$t" -lt "$max_s" ]]; do
    if out="$(snapshot 2>/dev/null)"; then
      if echo "$out" | grep -Fq "uri=${uri}"; then
        echo "$out"
        echo "[OK] alarm present for ${label}"
        return 0
      fi
    fi
    sleep "$poll_s"
    t=$((t+poll_s))
  done

  echo "[debug] last snapshot:"
  echo "$out" || true
  echo "[debug] central log tail:"; tail -n 120 tests/_central.log || true
  echo "[debug] regional log tail:"; tail -n 200 tests/_regional.log || true
  echo "[debug] lamp log tail:"; tail -n 200 tests/_lamp.log || true
  die "Expected alarm for ${label} not observed"
}

# ====== PRECHECK ======
[[ -f "$CERT" ]] || die "Missing cert: $CERT"
[[ -f "$KEY"  ]] || die "Missing key: $KEY"

if ss -ltn 2>/dev/null | awk '{print $4}' | grep -qE ":${CENTRAL_PORT}\$"; then
  die "CENTRAL_PORT ${CENTRAL_PORT} already in use (close old servers)"
fi
if ss -ltn 2>/dev/null | awk '{print $4}' | grep -qE ":${TLS_PORT}\$"; then
  die "TLS_PORT ${TLS_PORT} already in use (close old servers)"
fi

echo "[info] starting central_server..."
./central_server "$CENTRAL_PORT" "$CERT" "$KEY" > tests/_central.log 2>&1 &
CENTRAL_PID=$!
sleep 0.5
require_running "$CENTRAL_PID" "central_server" "tests/_central.log"
wait_listen "$CENTRAL_PORT" 50 || die "central did not listen on ${CENTRAL_PORT}"
echo "[info] central up"

echo "[info] starting regional_server..."
./regional_server 1 "$TLS_PORT" "$UDP_PORT" 127.0.0.1 "$CENTRAL_PORT" "$SYNC_INTERVAL_S" "$CERT" "$KEY" "$COMM_LOST_TIMEOUT_S" \
  > tests/_regional.log 2>&1 &
REGION_PID=$!
sleep 0.5
require_running "$REGION_PID" "regional_server" "tests/_regional.log"
wait_listen "$TLS_PORT" 50 || die "regional did not listen on TLS ${TLS_PORT}"
echo "[info] regional up"

echo "[info] starting luminaire_client (fault_ppm=1000)..."
./luminaire_client 127.0.0.1 "$TLS_PORT" "$LAMP_URI" "$ZONE" 25 0 2 1000 \
  > tests/_lamp.log 2>&1 &
LAMP_PID=$!

echo "[info] starting sensor_udp (NO fault, just motion to trigger CMD)..."
./sensor_udp 127.0.0.1 "$UDP_PORT" "$SENSOR_URI" "$ZONE" 20 0 25 10 1 \
  > tests/_sensor.log 2>&1 &
SENSOR_PID=$!

wait_alarm_uri "$LAMP_URI" "LAMP_FAULT" "$WAIT_MAX_S" "$POLL_S"
echo "[PASS] test_06a_alarm_lamp.sh"
