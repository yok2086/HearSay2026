#!/usr/bin/env bash
set -euo pipefail

JUMP_HOST="nathan@stujump.comp.nus.edu.sg"
ULTRA_HOST="xilinx@makerslab-fpga-36.ddns.comp.nus.edu.sg"

REMOTE_CONF="/tmp/mosquitto.conf"
REMOTE_BROKER_LOG="/tmp/mosquitto.log"
REMOTE_INFERENCE_SCRIPT="/home/xilinx/ultra96_inference.py"
REMOTE_INFERENCE_LOG="/tmp/ultra96_inference.log"

LOCAL_BIND="0.0.0.0"
LOCAL_PORT="8883"
REMOTE_PORT="8883"

LOCAL_DIR="$HOME/HearSay2026/Comms"
PYTHON_BIN="$HOME/miniconda3/bin/python3"

TMUX_SESSION="hearsay"

SSH_MUX_OPTS=(-o ControlMaster=auto -o ControlPath=/tmp/hearsay_ssh_mux_%r@%h:%p -o ControlPersist=10m)


cleanup() {
  echo ""
  echo "=== Shutting everything down ==="

  echo "Killing broker + inference script on the Ultra96..."
  ssh -J "$JUMP_HOST" "${SSH_MUX_OPTS[@]}" "$ULTRA_HOST" \
    "pkill -f '[m]osquitto -c $REMOTE_CONF' || true; pkill -f '[u]ltra96_inference.py' || true" \
    2>/dev/null || true

  echo "Closing local relay tunnel..."
  lsof -ti :"$LOCAL_PORT" 2>/dev/null | xargs -r kill 2>/dev/null || true

  echo "Killing tmux session '$TMUX_SESSION'..."
  tmux kill-session -t "$TMUX_SESSION" 2>/dev/null || true

  echo "Done. Everything stopped (Ultra96 included)."
}
# Runs on Ctrl+C AND on normal script exit (e.g. after you detach from tmux
# with Ctrl+B D, tmux attach returns and the script naturally exits here too).
trap cleanup EXIT

echo "=== 1. Checking/starting broker on Ultra96 ==="
ALREADY_RUNNING=$(ssh -J "$JUMP_HOST" "${SSH_MUX_OPTS[@]}" "$ULTRA_HOST" \
  "pgrep -f '[m]osquitto -c $REMOTE_CONF' || true")

if [ -n "$ALREADY_RUNNING" ]; then
  echo "Broker already running (pid $ALREADY_RUNNING) - skipping start, going straight to local scripts."
else
  echo "Starting broker..."
  ssh -J "$JUMP_HOST" "${SSH_MUX_OPTS[@]}" "$ULTRA_HOST" \
    "setsid nohup mosquitto -c $REMOTE_CONF -v > $REMOTE_BROKER_LOG 2>&1 < /dev/null &"
  sleep 2
fi

echo ""
echo "=== 2. Checking/starting DPU inference script on Ultra96 ==="

# Inference using sensor data
# INFERENCE_RUNNING=$(ssh -J "$JUMP_HOST" "${SSH_MUX_OPTS[@]}" "$ULTRA_HOST" \
#   "pgrep -f '[u]ltra96_inference.py' || true")

# Inference using dummy data
INFERENCE_RUNNING=$(ssh -J "$JUMP_HOST" "${SSH_MUX_OPTS[@]}" "$ULTRA_HOST" \
  "pgrep -f '[d]ummy_inference.py' || true")

if [ -n "$INFERENCE_RUNNING" ]; then
  echo "Inference script already running (pid $INFERENCE_RUNNING) - skipping start, going straight to local scripts."
else
  echo "Starting inference script..."
  ssh -J "$JUMP_HOST" "${SSH_MUX_OPTS[@]}" "$ULTRA_HOST" \
    "setsid nohup python3 $REMOTE_INFERENCE_SCRIPT > $REMOTE_INFERENCE_LOG 2>&1 < /dev/null &"
  sleep 2
fi

echo ""
echo "=== 3. Opening local relay tunnel (background) ==="
# Kill any stale tunnel already holding this port before opening a fresh one
lsof -ti :"$LOCAL_PORT" | xargs -r kill 2>/dev/null || true
sleep 1

ssh -J "$JUMP_HOST" "${SSH_MUX_OPTS[@]}" -f -N \
  -L "${LOCAL_BIND}:${LOCAL_PORT}:localhost:${REMOTE_PORT}" "$ULTRA_HOST"
echo "Tunnel opened in background."
sleep 1

echo ""
echo "=== 4. Launching coordinator.py & dashboard.py in tmux ==="

tmux kill-server 2>/dev/null || true
sleep 0.5

tmux new-session -d -s "$TMUX_SESSION" -n coordinator \
  "cd $LOCAL_DIR && $PYTHON_BIN coordinator.py; echo 'Press enter to close...'; read"
tmux new-window -t "$TMUX_SESSION" -n dashboard \
  "cd $LOCAL_DIR && $PYTHON_BIN dashboard.py; echo 'Press enter to close...'; read"
tmux select-window -t "$TMUX_SESSION:dashboard"

echo ""
echo "=== All set ==="
echo "Broker + inference: running on the Ultra96"
echo "Tunnel: running in the background on your Mac"
echo "coordinator / ble_bridge / dashboard: running inside tmux session '$TMUX_SESSION'"
echo ""
echo "Now power on the FireBeetle."
echo ""
echo "Attaching to tmux now - use Ctrl+B then a number key (or Ctrl+B, n) to switch windows."
echo ""
echo "IMPORTANT - how shutdown works:"
echo "  Ctrl+C while attached to tmux only stops whichever pane is focused (e.g. just ble_bridge.py)."
echo "  To fully stop EVERYTHING (including the Ultra96's broker and inference script):"
echo "    1) Detach from tmux with Ctrl+B then D"
echo "    2) That returns control to this script, which then runs full cleanup automatically."
tmux attach -t "$TMUX_SESSION"