#!/usr/bin/env bash
# Send the test clip to the running Whisper API server (in a second terminal).
#   ./test_server.sh [file.wav] [port]
set -euo pipefail
cd "$(dirname "$0")"
WAV="${1:-test_jfk.wav}"
PORT="${2:-8090}"
time curl -s --noproxy '*' "http://127.0.0.1:$PORT/inference" -F "file=@$WAV" -F response_format=json
echo
