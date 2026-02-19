#!/usr/bin/env bash
set -euo pipefail

CENTRAL_PORT="${CENTRAL_PORT:-6000}"
REGION_ID="${REGION_ID:-1}"
REG_TLS_PORT="${REG_TLS_PORT:-5555}"
REG_UDP_PORT="${REG_UDP_PORT:-7777}"
SYNC_INTERVAL="${SYNC_INTERVAL:-5}"
COMM_LOST_TIMEOUT="${COMM_LOST_TIMEOUT:-20}"

CERT="${CERT:-cert.pem}"
KEY="${KEY:-key.pem}"

mkdir -p logs

# (opcionalno) obriši stare logove
rm -f logs/central.log logs/regional.log

echo "[run_stack] starting central_server..."
./central_server "${CENTRAL_PORT}" "${CERT}" "${KEY}" > logs/central.log 2>&1 &
echo $! > logs/central.pid

sleep 0.3

echo "[run_stack] starting regional_server..."
./regional_server "${REGION_ID}" "${REG_TLS_PORT}" "${REG_UDP_PORT}" 127.0.0.1 "${CENTRAL_PORT}" \
  "${SYNC_INTERVAL}" "${CERT}" "${KEY}" "${COMM_LOST_TIMEOUT}" > logs/regional.log 2>&1 &
echo $! > logs/regional.pid

echo "[run_stack] OK. logs in ./logs/"