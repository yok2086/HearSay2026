#!/usr/bin/env bash
# Transcribe an audio file:  ./transcribe.sh recording.wav  [model]
# The file must be 16 kHz mono WAV. Convert anything else with:
#   ffmpeg -i input.m4a -ar 16000 -ac 1 -c:a pcm_s16le output.wav
set -euo pipefail
cd "$(dirname "$0")"
WAV="${1:?usage: ./transcribe.sh file.wav [models/ggml-tiny.en.bin]}"
MODEL="${2:-models/ggml-tiny.en.bin}"
THREADS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
# -nt: no timestamps   -sns: drop "[Music]"-style non-speech tokens
./whisper.cpp/build/bin/whisper-cli -m "$MODEL" -f "$WAV" -l en -t "$THREADS" -nt -sns 2>/dev/null
