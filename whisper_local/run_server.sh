#!/usr/bin/env bash
# Whisper speech-to-text API server (run this on the Ultra96).
#   ./run_server.sh [model] [port]
# The phone app sends short WAV clips to http://127.0.0.1:<port>/inference through
# its SSH tunnel and gets the text back as JSON: {"text": "..."}.
# Default port 8090 (8080 is already used by the HearSay dummy server tunnel).
# Leave it running; stop with Ctrl+C. To keep it running after you log out:
#   mkdir -p ~/logs; setsid nohup ./run_server.sh > ~/logs/whisper.log 2>&1 < /dev/null &
# Default model: tiny.en (fastest English model). Uses its 8-bit version (made by
# build.sh) when it exists, and falls back to base.en if tiny.en isn't on the board.
# Speed options: -nt no timestamps, -nf no temperature fallback retries,
# -bo 1 -bs 1 single greedy pass, -ac 512 (~10 s audio window; clips are <= 6 s).
set -euo pipefail
cd "$(dirname "$0")"
DEFAULT_MODEL=models/ggml-base.en.bin
[ -f models/ggml-base.en-q8_0.bin ] && DEFAULT_MODEL=models/ggml-base.en-q8_0.bin
[ -f models/ggml-tiny.en.bin ] && DEFAULT_MODEL=models/ggml-tiny.en.bin
[ -f models/ggml-tiny.en-q8_0.bin ] && DEFAULT_MODEL=models/ggml-tiny.en-q8_0.bin
MODEL="${1:-$DEFAULT_MODEL}"
PORT="${2:-8090}"
THREADS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
echo "Whisper API on 127.0.0.1:$PORT using $MODEL ($THREADS threads)"
exec ./whisper.cpp/build/bin/whisper-server -m "$MODEL" --host 127.0.0.1 --port "$PORT" -t "$THREADS" -l en -sns -nt -nf -bo 1 -bs 1 -ac 512
