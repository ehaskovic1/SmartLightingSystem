#!/usr/bin/env bash
set -euo pipefail

CENTRAL_PORT="${CENTRAL_PORT:-6000}"
TLS_PORT="${TLS_PORT:-5555}"
UDP_PORT="${UDP_PORT:-7777}"
SYNC_INTERVAL_S="${SYNC_INTERVAL_S:-1}"
COMM_LOST_TIMEOUT_S="${COMM_LOST_TIMEOUT_S:-6}"  # brže

ZONE="${ZONE:-1}"
LAMP_URI="${LAMP_URI:-sls://lamp-001}"

CERT="${CERT:-cert.pem}"
KEY="${KEY:-key.pem}"

LAMP_RUN_S="${LAMP_RUN_S:-60}"
LAMP_RESET_S="${LAMP_RESET_S:-0}"
LAMP_HEARTBEAT_S="${LAMP_HEARTBEAT_S:-1}"
LAMP_FAULT_PPM="${LAMP_FAULT_PPM:-0}"

BOOT_WAIT_S="${BOOT_WAIT_S:-1}"
PORT_WAIT_S="${PORT_WAIT_S:-10}"

mkdir -p tests

die(){ echo "[FAIL] $*" >&2; exit 1; }

cleanup(){
  set +e
  echo "[cleanup] stopping processes..."
  for pid in "${LAMP_PID:-}" "${REGION_PID:-}" "${CENTRAL_PID:-}"; do
    [[ -n "${pid}" ]] && kill -TERM "$pid" 2>/dev/null || true
  done
  sleep 0.5
  for pid in "${LAMP_PID:-}" "${REGION_PID:-}" "${CENTRAL_PID:-}"; do
    [[ -n "${pid}" ]] && kill -KILL "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

kill_ports(){
  set +e
  fuser -k -n tcp "$CENTRAL_PORT" 2>/dev/null || true
  fuser -k -n tcp "$TLS_PORT" 2>/dev/null || true
  fuser -k -n udp "$UDP_PORT" 2>/dev/null || true
  set -e
}

wait_for_port(){
  local port="$1"
  local max_s="$2"
  python3 - "$port" "$max_s" <<'PY'
import socket, sys, time
port = int(sys.argv[1]); max_s = int(sys.argv[2])
t0 = time.time()
while True:
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=0.3)
        s.close()
        sys.exit(0)
    except Exception:
        pass
    if time.time() - t0 > max_s:
        sys.exit(1)
    time.sleep(0.2)
PY
}

require_running_or_die(){
  local pid="$1" name="$2" log="$3"
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "[debug] ${name} exited early. Tail of ${log}:"
    tail -n 260 "$log" 2>/dev/null || true
    die "${name} not running"
  fi
}

wait_lamp_register_or_die(){
  local timeout_s="${1:-18}"
  python3 - "$timeout_s" <<'PY'
import time, sys
timeout_s = int(sys.argv[1])
deadline = time.time() + timeout_s
path = "tests/_lamp.log"

def tail(n=200):
    try:
        s = open(path,"r",errors="ignore").read().splitlines()
        return "\n".join(s[-n:])
    except Exception:
        return ""

while time.time() < deadline:
    s = tail(240)
    if ("REGISTERING -> OFF" in s) and ("REGISTER_ACK" in s):
        print("[OK] lamp registered")
        raise SystemExit(0)
    time.sleep(0.2)

print("[FAIL] lamp did not register. Tail:")
print(tail(280))
raise SystemExit(1)
PY
}

wait_comm_lost_or_die(){
  local uri="$1"
  local timeout_s="$2"
  python3 - "$uri" "$timeout_s" <<'PY'
import sys, time
uri = sys.argv[1]
timeout_s = int(sys.argv[2])
deadline = time.time() + timeout_s
path = "tests/_regional.log"

def tail(n=500):
    try:
        s = open(path,"r",errors="ignore").read().splitlines()
        return "\n".join(s[-n:])
    except Exception:
        return ""

while time.time() < deadline:
    s = tail(700)
    if ("comm_lost" in s) and (uri in s):
        print("[OK] comm_lost observed for", uri)
        raise SystemExit(0)
    time.sleep(0.3)

print("[FAIL] comm_lost not observed. Tail:")
print(tail(800))
raise SystemExit(1)
PY
}

# ===== PRECHECKS =====
[[ -x ./central_server ]]   || die "Missing ./central_server"
[[ -x ./regional_server ]]  || die "Missing ./regional_server"
[[ -x ./luminaire_client ]] || die "Missing ./luminaire_client"
[[ -f "$CERT" ]] || die "Missing cert: $CERT"
[[ -f "$KEY"  ]] || die "Missing key: $KEY"
command -v stdbuf >/dev/null 2>&1 || die "stdbuf not found (install coreutils)"

kill_ports

echo "[info] starting central_server..."
stdbuf -oL -eL ./central_server "$CENTRAL_PORT" "$CERT" "$KEY" > tests/_central.log 2>&1 &
CENTRAL_PID=$!
sleep "$BOOT_WAIT_S"
require_running_or_die "$CENTRAL_PID" "central_server" "tests/_central.log"
wait_for_port "$CENTRAL_PORT" "$PORT_WAIT_S" || { tail -n 240 tests/_central.log || true; die "central did not open port"; }
echo "[info] central up"

echo "[info] starting regional_server..."
stdbuf -oL -eL ./regional_server 1 "$TLS_PORT" "$UDP_PORT" 127.0.0.1 "$CENTRAL_PORT" "$SYNC_INTERVAL_S" "$CERT" "$KEY" "$COMM_LOST_TIMEOUT_S" \
  > tests/_regional.log 2>&1 &
REGION_PID=$!
sleep "$BOOT_WAIT_S"
require_running_or_die "$REGION_PID" "regional_server" "tests/_regional.log"
wait_for_port "$TLS_PORT" "$PORT_WAIT_S" || { tail -n 320 tests/_regional.log || true; die "regional did not open TLS port"; }
echo "[info] regional up"

echo "[info] starting luminaire_client..."
stdbuf -oL -eL ./luminaire_client 127.0.0.1 "$TLS_PORT" "$LAMP_URI" "$ZONE" "$LAMP_RUN_S" "$LAMP_RESET_S" "$LAMP_HEARTBEAT_S" "$LAMP_FAULT_PPM" \
  > tests/_lamp.log 2>&1 &
LAMP_PID=$!

echo "[test_05] waiting registration..."
wait_lamp_register_or_die 20

echo "[test_05] killing lamp to force comm_lost..."
kill -KILL "$LAMP_PID" 2>/dev/null || true
LAMP_PID=""

WAIT_S=$((COMM_LOST_TIMEOUT_S + 15))  # comm_lost tick je svakih 5s u tvom kodu
echo "[test_05] waiting up to ${WAIT_S}s for comm_lost..."
wait_comm_lost_or_die "$LAMP_URI" "$WAIT_S"

echo "[PASS] test_05_comm_lost_lamp"
