#!/usr/bin/env bash
set -euo pipefail

JUMP_HOST="nathan@stujump.comp.nus.edu.sg"
ULTRA_HOST="xilinx@makerslab-fpga-36.ddns.comp.nus.edu.sg"

REMOTE_MQTT_DIR="/home/xilinx/mqtt"
REMOTE_CONF="$REMOTE_MQTT_DIR/mosquitto.conf"
REMOTE_LOG_DIR="/home/xilinx/logs"
REMOTE_BROKER_LOG="$REMOTE_LOG_DIR/mosquitto.log"

# Dummy Inference
# REMOTE_INFERENCE_SCRIPT="/home/xilinx/dummy_inference.py"

# TCN inference on the DPU (two gloves)
REMOTE_INFERENCE_SCRIPT="/home/xilinx/jupyter_notebooks/tcn/tcn_handler.py"

# Dummy Phone sender
# REMOTE_INFERENCE_SCRIPT="/home/xilinx/dummy_phone_sender.py"

# Sensor Inference
# REMOTE_INFERENCE_SCRIPT="/home/xilinx/ultra96_inference.py"

# Keyboard pipeline test
# REMOTE_INFERENCE_SCRIPT="/home/xilinx/keyboard_ultra96.py"

REMOTE_INFERENCE_LOG="$REMOTE_LOG_DIR/ultra96_inference.log"

# Whisper speech-to-text server (whisper_local/run_server.sh, port 8090; the phone reaches it over its SSH tunnel)
REMOTE_WHISPER_DIR="/home/xilinx/whisper_local"
REMOTE_WHISPER_LOG="$REMOTE_LOG_DIR/whisper.log"

REMOTE_PYTHON="sudo -n /usr/local/share/pynq-venv/bin/python3 -u"
# REMOTE_PYTHON="python3 -u"

LOCAL_BIND="0.0.0.0"
LOCAL_PORT="8883"
REMOTE_PORT="8883"
# Hotspot backup: gloves/headset on the same hotspot send plain MQTT here
RELAY_PORT="1883"

LOCAL_DIR="$HOME/HearSay2026/Comms"
PYTHON_BIN="$HOME/miniconda3/bin/python3"

TMUX_SESSION="hearsay"
# The pipeline's own tmux server (socket "hearsay"), so it never touches your other
# tmux sessions and a stuck default server can't stop it from starting.
htmux() { tmux -L hearsay "$@"; }

SSH_MUX_OPTS=(-o ControlMaster=auto -o ControlPath=$HOME/.ssh/hearsay_mux_%C -o ControlPersist=10m)


cleanup() {
  echo ""
  echo "=== Shutting everything down ==="

  echo "Killing broker + inference script + Whisper server on the Ultra96..."
  ssh -J "$JUMP_HOST" "${SSH_MUX_OPTS[@]}" "$ULTRA_HOST" \
    "pkill -f '[m]osquitto -c $REMOTE_CONF' || true; \
     pkill -f '[u]ltra96_inference.py' || true; \
     pkill -f '[d]ummy_inference.py' || true; \
     pkill -f '[d]ummy_phone_sender.py' || true; \
     pkill -f '[k]eyboard_ultra96.py' || true; \
     pkill -f '[w]hisper-server' || true; \
     sudo -n pkill -f '[t]cn_handler.py' || true" \
    2>/dev/null || true

  echo "Closing local relay tunnel..."
  lsof -ti :"$LOCAL_PORT" 2>/dev/null | xargs -r kill 2>/dev/null || true
  lsof -ti :"$RELAY_PORT" 2>/dev/null | xargs -r kill 2>/dev/null || true

  echo "Killing tmux session '$TMUX_SESSION'..."
  htmux kill-server 2>/dev/null || true

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
    "mkdir -p $REMOTE_LOG_DIR; setsid nohup mosquitto -c $REMOTE_CONF > $REMOTE_BROKER_LOG 2>&1 < /dev/null &"
  sleep 2
fi

echo ""
echo "=== 2. Starting DPU inference script on Ultra96 ==="

# Inference using sensor data
# INFERENCE_RUNNING=$(ssh -J "$JUMP_HOST" "${SSH_MUX_OPTS[@]}" "$ULTRA_HOST" \
#   "pgrep -f '[u]ltra96_inference.py' || true")

# Inference using dummy data
# INFERENCE_RUNNING=$(ssh -J "$JUMP_HOST" "${SSH_MUX_OPTS[@]}" "$ULTRA_HOST" \
#     "pgrep -f '[d]ummy_inference.py' || true")

# TCN inference on the DPU
INFERENCE_RUNNING=$(ssh -J "$JUMP_HOST" "${SSH_MUX_OPTS[@]}" "$ULTRA_HOST" \
    "pgrep -f '[t]cn_handler.py' || true")

# Dummy phone sender data
# INFERENCE_RUNNING=$(ssh -J "$JUMP_HOST" "${SSH_MUX_OPTS[@]}" "$ULTRA_HOST" \
#   "pgrep -f '[d]ummy_phone_sender.py' || true")

# Keyboard pipeline test
# INFERENCE_RUNNING=$(ssh -J "$JUMP_HOST" "${SSH_MUX_OPTS[@]}" "$ULTRA_HOST" \
#   "pgrep -f '[k]eyboard_ultra96.py' || true")

if [ -n "$INFERENCE_RUNNING" ]; then
  echo "Inference script already running (pid $INFERENCE_RUNNING) - skipping start, going straight to local scripts."
else
  echo "Starting inference script..."
  ssh -J "$JUMP_HOST" "${SSH_MUX_OPTS[@]}" "$ULTRA_HOST" \
    "mkdir -p $REMOTE_LOG_DIR; setsid nohup $REMOTE_PYTHON $REMOTE_INFERENCE_SCRIPT > $REMOTE_INFERENCE_LOG 2>&1 < /dev/null &"
  sleep 2
fi

# echo ""
# echo "=== 3. Starting Whisper server on Ultra96 ==="
# WHISPER_RUNNING=$(ssh -J "$JUMP_HOST" "${SSH_MUX_OPTS[@]}" "$ULTRA_HOST" \
#     "pgrep -f '[w]hisper-server' || true")
# 
# if [ -n "$WHISPER_RUNNING" ]; then
#   echo "Whisper server already running (pid $WHISPER_RUNNING) - skipping start."
# else
#   echo "Starting Whisper server..."
#   ssh -J "$JUMP_HOST" "${SSH_MUX_OPTS[@]}" "$ULTRA_HOST" \
#     "mkdir -p $REMOTE_LOG_DIR; if [ -x $REMOTE_WHISPER_DIR/run_server.sh ]; then \
#        setsid nohup $REMOTE_WHISPER_DIR/run_server.sh > $REMOTE_WHISPER_LOG 2>&1 < /dev/null & \
#      else echo 'WARNING: $REMOTE_WHISPER_DIR/run_server.sh not found - captions on the Ultra96 will not work'; fi"
#   sleep 2
# fi

echo ""
echo "=== 4. Opening local relay tunnel (background) ==="
# Kill any stale tunnel already holding this port before opening a fresh one
lsof -ti :"$LOCAL_PORT" | xargs -r kill 2>/dev/null || true
lsof -ti :"$RELAY_PORT" | xargs -r kill 2>/dev/null || true
sleep 1

ssh -J "$JUMP_HOST" "${SSH_MUX_OPTS[@]}" -f -N \
  -L "${LOCAL_BIND}:${LOCAL_PORT}:localhost:${REMOTE_PORT}" \
  -L "${LOCAL_BIND}:${RELAY_PORT}:localhost:1883" "$ULTRA_HOST"
echo "Tunnel opened in background (Mac ${LOCAL_PORT} -> Ultra96 broker)."
echo "Hotspot relay open on port ${RELAY_PORT} for devices on the same network."
sleep 1

echo ""
echo "=== 5. Launching coordinator.py & dashboard.py & stats.py in tmux ==="

htmux kill-server 2>/dev/null || true
sleep 0.5
# A server that crashed can leave a socket nothing answers on ("server exited
# unexpectedly"); remove it so new-session starts a fresh server.
if ! htmux list-sessions >/dev/null 2>&1; then
  rm -f "${TMUX_TMPDIR:-/tmp}/tmux-$(id -u)/hearsay"
fi

htmux new-session -d -s "$TMUX_SESSION" -n coordinator \
  "cd $LOCAL_DIR && $PYTHON_BIN coordinator.py; echo 'Press enter to close...'; read"
htmux new-window -t "$TMUX_SESSION" -n dashboard \
  "cd $LOCAL_DIR && $PYTHON_BIN dashboard.py; echo 'Press enter to close...'; read"
htmux new-window -t "$TMUX_SESSION" -n stats \
  "cd $LOCAL_DIR && $PYTHON_BIN stats.py; echo 'Press enter to close...'; read"

# Keyboard pipeline test: relay window
# htmux new-window -t "$TMUX_SESSION" -n keyboard \
  # "cd $LOCAL_DIR && $PYTHON_BIN keyboard_relay.py; echo 'Press enter to close...'; read"

htmux select-window -t "$TMUX_SESSION:dashboard"

echo ""
echo "=== All set ==="
echo "Broker + inference: running on the Ultra96 (Whisper startup is commented out)"
echo "Tunnel running in the background"
echo "coordinator / dashboard / stats running inside tmux session '$TMUX_SESSION'"
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
htmux attach -t "$TMUX_SESSION"