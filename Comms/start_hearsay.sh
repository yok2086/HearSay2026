#!/usr/bin/env bash
set -euo pipefail

# ---- Adjust these to match your setup ----
JUMP_HOST="nathan@stujump.comp.nus.edu.sg"
ULTRA_HOST="xilinx@makerslab-fpga-36.ddns.comp.nus.edu.sg"
REMOTE_CONF="/tmp/mosquitto.conf"
REMOTE_LOG="/tmp/mosquitto.log"
LOCAL_BIND="0.0.0.0"
LOCAL_PORT="8883"
REMOTE_PORT="8883"

# SSH connection multiplexing
SSH_MUX_OPTS=(-o ControlMaster=auto -o ControlPath=/tmp/hearsay_ssh_mux_%r@%h:%p -o ControlPersist=10m)
# -------------------------------------------

echo "Checking if the broker is already running on the Ultra96..."
ALREADY_RUNNING=$(ssh -J "$JUMP_HOST" "${SSH_MUX_OPTS[@]}" "$ULTRA_HOST" \
  "pgrep -f '[m]osquitto -c $REMOTE_CONF' || true")

if [ -n "$ALREADY_RUNNING" ]; then
  echo "Broker already running on the Ultra96 (pid $ALREADY_RUNNING) — leaving it as is."
else
  echo "Starting the broker on the Ultra96..."
  ssh -J "$JUMP_HOST" "${SSH_MUX_OPTS[@]}" "$ULTRA_HOST" \
    "setsid nohup mosquitto -c $REMOTE_CONF -v > $REMOTE_LOG 2>&1 < /dev/null &"
  sleep 2
  echo "Broker start command sent. Recent log output:"
  ssh -J "$JUMP_HOST" "${SSH_MUX_OPTS[@]}" "$ULTRA_HOST" "tail -n 10 $REMOTE_LOG || true"
fi

echo ""
echo "Opening local relay tunnel: ${LOCAL_BIND}:${LOCAL_PORT} -> Ultra96's localhost:${REMOTE_PORT}"
echo "Press Ctrl+C to stop the tunnel. The broker on the Ultra96 will keep running afterward."
echo ""

exec ssh -J "$JUMP_HOST" "${SSH_MUX_OPTS[@]}" -N -L "${LOCAL_BIND}:${LOCAL_PORT}:localhost:${REMOTE_PORT}" "$ULTRA_HOST"