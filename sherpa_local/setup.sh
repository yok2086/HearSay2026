#!/usr/bin/env bash
# One-time setup on the Ultra96 (needs internet): installs sherpa-onnx and
# downloads the English streaming model into models/.
#   ./setup.sh            bigger, more accurate model (2023-06-26)
#   ./setup.sh small      small, faster model (20M, 2023-02-17)
set -euo pipefail
cd "$(dirname "$0")"

python3 -m pip install --user --upgrade sherpa-onnx numpy

if [ "${1:-}" = "small" ]; then
  MODEL=sherpa-onnx-streaming-zipformer-en-20M-2023-02-17
else
  MODEL=sherpa-onnx-streaming-zipformer-en-2023-06-26
fi
mkdir -p models
if [ ! -d "models/$MODEL" ]; then
  curl -L -o "models/$MODEL.tar.bz2" \
    "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/$MODEL.tar.bz2"
  tar -xjf "models/$MODEL.tar.bz2" -C models
  rm -f "models/$MODEL.tar.bz2"
  rm -rf "models/$MODEL/test_wavs"
fi
python3 -c "import sherpa_onnx; print('sherpa-onnx', sherpa_onnx.__version__, 'ready')"
echo "Model: models/$MODEL"
