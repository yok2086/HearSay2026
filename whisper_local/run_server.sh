#!/usr/bin/env bash
# Whisper speech-to-text API server (run this on the Ultra96).
#   ./run_server.sh [model] [port]
# The phone app sends short WAV clips to http://127.0.0.1:<port>/inference through
# its SSH tunnel and gets the text back as JSON: {"text": "..."}.
# Default port 8090 (8080 is already used by the HearSay dummy server tunnel).
# Leave it running; stop with Ctrl+C. To keep it running after you log out:
#   nohup ./run_server.sh > whisper_server.log 2>&1 &
set -euo pipefail
cd "$(dirname "$0")"
MODEL="${1:-models/ggml-base.en.bin}"
PORT="${2:-8090}"
THREADS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
echo "Whisper API on 127.0.0.1:$PORT using $MODEL ($THREADS threads)"
exec ./whisper.cpp/build/bin/whisper-server -m "$MODEL" --host 127.0.0.1 --port "$PORT" -t "$THREADS" -l en -sns
