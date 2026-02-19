
#!/usr/bin/env bash
set -euo pipefail

HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-5555}"
N="${N:-100}"
ZONE="${ZONE:-1}"
RUN_S="${RUN_S:-8}"        # dovoljno da se stignu registrovati
HEARTBEAT_S="${HEARTBEAT_S:-2}"

mkdir -p logs
rm -f logs/lamp_*.log logs/registration_metrics.txt

echo "[test_registration_100] starting ${N} lamps..."
t0_ms=$(date +%s%3N)

for i in $(seq 1 "$N"); do
  uri="sls://lamp-$(printf "%03d" "$i")"
  ./luminaire_client "$HOST" "$PORT" "$uri" "$ZONE" "$RUN_S" 0 "$HEARTBEAT_S" 0 \
    > "logs/lamp_${i}.log" 2>&1 &
done

wait || true
t1_ms=$(date +%s%3N)

# Koliko ih je dobilo REGISTER_ACK?
ack_cnt=$(grep -R "REGISTER_ACK" -n logs/lamp_*.log | wc -l | tr -d ' ')
echo "ACK_CNT=$ack_cnt" | tee logs/registration_metrics.txt
echo "TOTAL=$N" | tee -a logs/registration_metrics.txt
echo "WALL_MS=$((t1_ms - t0_ms))" | tee -a logs/registration_metrics.txt

echo "[test_registration_100] done. see logs/registration_metrics.txt"