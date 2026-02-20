#!/usr/bin/env bash
set -euo pipefail

CENTRAL_PORT="${CENTRAL_PORT:-6000}"
TLS_PORT="${TLS_PORT:-5555}"
UDP_PORT="${UDP_PORT:-7777}"
SYNC_INTERVAL_S="${SYNC_INTERVAL_S:-1}"
COMM_LOST_TIMEOUT_S="${COMM_LOST_TIMEOUT_S:-20}"

CERT="${CERT:-cert.pem}"
KEY="${KEY:-key.pem}"

mkdir -p tests/_tmp

die(){ echo "[FAIL] $*" >&2; exit 1; }

now_ms(){ python3 - <<'PY'
import time; print(int(time.time()*1000))
PY
}

wait_for_port(){
  local port="$1"; local max_s="${2:-10}"
  python3 - "$port" "$max_s" <<'PY'
import socket, sys, time
port=int(sys.argv[1]); max_s=int(sys.argv[2])
t0=time.time()
while True:
  try:
    s=socket.create_connection(("127.0.0.1", port), timeout=0.3)
    s.close(); sys.exit(0)
  except Exception:
    pass
  if time.time()-t0>max_s: sys.exit(1)
  time.sleep(0.2)
PY
}

cleanup_pids(){
  set +e
  for pid in "$@"; do
    [[ -n "${pid:-}" ]] && kill -TERM "$pid" 2>/dev/null || true
  done
  sleep 0.5
  for pid in "$@"; do
    [[ -n "${pid:-}" ]] && kill -KILL "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
}

require_running(){
  local pid="$1"; local name="$2"; local log="$3"
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "[debug] ${name} exited early. Tail of ${log}:"
    tail -n 120 "$log" 2>/dev/null || true
    die "${name} not running"
  fi
}

snapshot(){
  ./admin_cli 127.0.0.1 "$CENTRAL_PORT"
}

alarm_count_from_snapshot(){
  # bez grep specijalnih opcija: samo awk
  awk '
    /--- ALARMS \(/ {
      s=$0
      gsub(/[^0-9]/,"",s)
      print s
      exit
    }
    END { print 0 }
  '
}

# Generiše i kompajlira BINARNI UDP sender koji koristi proto.hpp (TelemetryUdp)
build_udp_sender(){
  cat > tests/_tmp/udp_send_bin.cpp <<'CPP'
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <string>
#include "proto.hpp"

int main(int argc, char** argv){
  if(argc < 6){
    std::cerr << "Usage: udp_send_bin <host> <port> <uri> <zone> <motion>\n";
    std::cerr << "  motion: 0/1 normal, 254 = fault marker\n";
    return 2;
  }
  std::string host = argv[1];
  int port = std::stoi(argv[2]);
  std::string uri = argv[3];
  uint32_t zone = (uint32_t)std::stoul(argv[4]);
  int motion = std::stoi(argv[5]);

  sls::TelemetryUdp t{};
  std::memset(&t, 0, sizeof(t));
  std::snprintf(t.uri, sizeof(t.uri), "%s", uri.c_str());
  t.zone_id_be = htonl(zone);
  t.lux_be = htons(180);
  // temp 23.5C -> 235 (x10)
  uint16_t temp10 = (uint16_t)235;
  t.temp_c_x10_be = htons(temp10);
  t.motion = (uint8_t)motion;

  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if(fd < 0){ perror("socket"); return 1; }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  addr.sin_addr.s_addr = inet_addr(host.c_str());

  ssize_t n = ::sendto(fd, &t, sizeof(t), 0, (sockaddr*)&addr, sizeof(addr));
  if(n < 0){ perror("sendto"); close(fd); return 1; }

  std::cout << "sent TelemetryUdp bytes=" << n << " uri=" << uri
            << " zone=" << zone << " motion=" << motion << "\n";
  close(fd);
  return 0;
}
CPP

  g++ -std=c++17 -I . tests/_tmp/udp_send_bin.cpp -o tests/_tmp/udp_send_bin
}
