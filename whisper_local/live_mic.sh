#!/usr/bin/env bash
# Live captions from a microphone in the terminal (needs ./build.sh --with-mic).
#   ./live_mic.sh [model]        Ctrl+C to stop
# --step/--length are in ms: transcribe every 1.5 s using the last 8 s of audio,
# matching the Unity app's settings.
set -euo pipefail
cd "$(dirname "$0")"
MODEL="${1:-models/ggml-base.en.bin}"
THREADS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
./whisper.cpp/build/bin/whisper-stream -m "$MODEL" -l en -t "$THREADS" --step 1500 --length 8000 --keep 200 -vth 0.6
