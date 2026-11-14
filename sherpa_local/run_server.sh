#!/usr/bin/env bash
# Speech-to-text API server with sherpa-onnx (run this on the Ultra96).
#   ./run_server.sh [model-folder] [port]
# Same address and API as whisper_local/run_server.sh (127.0.0.1:8090,
# POST /inference), so the phone app works with either. Starting this one stops
# the Whisper server; start ~/whisper_local/run_server.sh to switch back.
# Keep it running after you log out:
#   setsid nohup ./run_server.sh > sherpa_server.log 2>&1 < /dev/null &
set -euo pipefail
cd "$(dirname "$0")"
MODEL="${1:-}"
PORT="${2:-8090}"
if [ -z "$MODEL" ]; then
  # Default: the bigger model if it was downloaded, otherwise the small one.
  for m in models/sherpa-onnx-streaming-zipformer-en-2023-06-26 models/sherpa-onnx-streaming-zipformer-en-20M-2023-02-17; do
    [ -d "$m" ] && MODEL="$m" && break
  done
fi
[ -n "$MODEL" ] || { echo "No model in models/ - run ./setup.sh first"; exit 1; }
# Only one server can use the port: stop Whisper (and an older sherpa) first.
pkill -f whisper-server 2>/dev/null || true
pkill -f sherpa_server.py 2>/dev/null || true
sleep 2
exec python3 -u sherpa_server.py --model "$MODEL" --port "$PORT" --decoding greedy_search
