#!/usr/bin/env bash
# Builds whisper.cpp (same engine as the Unity whisper plugin) for this machine.
# Works on macOS and on the Ultra96 (aarch64 Linux). Needs: cmake + a C++ compiler.
#   macOS:    xcode-select --install ; brew install cmake
#   Ultra96:  sudo apt-get install -y build-essential cmake
# Optional live-mic tool (whisper-stream) needs SDL2:
#   macOS: brew install sdl2     Ultra96: sudo apt-get install -y libsdl2-dev
set -euo pipefail
cd "$(dirname "$0")"
[ -d whisper.cpp ] || tar -xzf whisper.cpp-src.tar.gz
SDL=OFF
if [ "${1:-}" = "--with-mic" ]; then SDL=ON; fi
cmake -S whisper.cpp -B whisper.cpp/build -DCMAKE_BUILD_TYPE=Release -DWHISPER_BUILD_TESTS=OFF -DWHISPER_SDL2=$SDL
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
TARGETS="--target whisper-cli --target whisper-server --target whisper-quantize"
[ "$SDL" = ON ] && TARGETS="$TARGETS --target whisper-stream"
cmake --build whisper.cpp/build -j"$JOBS" --config Release $TARGETS
# 8-bit copy of the model: faster on the Ultra96's ARM cores, practically the same accuracy.
if [ -f models/ggml-base.en.bin ] && [ ! -f models/ggml-base.en-q8_0.bin ]; then
  ./whisper.cpp/build/bin/whisper-quantize models/ggml-base.en.bin models/ggml-base.en-q8_0.bin q8_0
fi
if [ -f models/ggml-tiny.en.bin ] && [ ! -f models/ggml-tiny.en-q8_0.bin ]; then
  ./whisper.cpp/build/bin/whisper-quantize models/ggml-tiny.en.bin models/ggml-tiny.en-q8_0.bin q8_0
fi
echo
echo "Built: $(pwd)/whisper.cpp/build/bin/"
ls whisper.cpp/build/bin
