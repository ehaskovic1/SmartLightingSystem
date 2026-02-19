#!/usr/bin/env bash
set -euo pipefail

kill_if() { [[ -f "$1" ]] && kill "$(cat "$1")" 2>/dev/null || true; }

kill_if logs/regional.pid
kill_if logs/central.pid

rm -f logs/*.pid
echo "[stop_stack] stopped."