# HearSay Whisper — local / terminal version

This is the same speech-to-text engine the Unity app uses (whisper.cpp, which the
`whisper.unity` plugin wraps), run from a terminal. It reads the same model files
(`ggml-*.bin`), so results should match what the phone app does.

- whisper.cpp source pinned at commit `284029c` (Oct 28 2026), in `whisper.cpp-src.tar.gz`
- `models/ggml-base.en.bin` is the model the Unity app now uses (English only, 148 MB)
- Tested: `./transcribe.sh test_jfk.wav` prints the expected sentence

## 1. Run it on the Mac

```bash
cd ~/HearSay2022/whisper_local
chmod +x *.sh                  # once
xcode-select --install         # once, if you have never installed the compiler
brew install cmake             # once
./build.sh                     # about 1–2 min
./transcribe.sh test_jfk.wav   # should print "...ask not what your country can do for you..."
```

Your own recording (any format → 16 kHz mono WAV first):

```bash
brew install ffmpeg            # once
ffmpeg -i myclip.m4a -ar 16000 -ac 1 -c:a pcm_s16le myclip.wav
./transcribe.sh myclip.wav
```

Live microphone captions in the terminal (optional):

```bash
brew install sdl2
./build.sh --with-mic
./live_mic.sh                  # Ctrl+C to stop; allow Terminal to use the mic
```

## 2. Copy it to the Ultra96 (Thursday, on the school VPN)

Copy the folder **without** the Mac build (it must be rebuilt on the board).
Use the same SSH route as `run_hearsay_pipeline.sh` (stujump → ultra96):

```bash
# from the Mac, with the VPN connected (replace user/host with yours)
cd ~/HearSay2022
rsync -av --exclude whisper.cpp/ whisper_local/ -e "ssh -J <user>@stujump" xilinx@<ultra96-ip>:~/whisper_local/
#   or, if rsync is not available:
# scp -r -o ProxyJump=<user>@stujump whisper_local xilinx@<ultra96-ip>:~/
#   (then on the board: rm -rf ~/whisper_local/whisper.cpp before building)
```

Then on the Ultra96:

```bash
cd ~/whisper_local
sudo apt-get install -y build-essential cmake   # if not already installed
chmod +x *.sh
./build.sh                     # slower on the board (several minutes)
./transcribe.sh test_jfk.wav
```

The Ultra96 doesn't need internet for this: the source and model are in the folder.

## 3. Speed on the Ultra96

The Ultra96's 4 small ARM cores are much slower than a Mac. If `base.en` is too
slow, use the smaller English model `tiny.en` (75 MB, faster, a bit less accurate).
Download it **on the Mac** (needs internet), then copy it over with the rest:

```bash
bash whisper.cpp/models/download-ggml-model.sh tiny.en models   # after ./build.sh on the Mac
./transcribe.sh test_jfk.wav models/ggml-tiny.en.bin
```

Time both with `time ./transcribe.sh test_jfk.wav [model]` (the clip is 11 s long:
anything under 11 s is faster than real time).

## Notes

- `-sns` in `transcribe.sh` suppresses non-speech tokens like `[Music]`.
- Audio must be 16 kHz mono for whisper.cpp. The phone app also records at 16 kHz.
- Don't put this folder inside `Assets/` (Unity would try to import it).
- `.gitignore` keeps the 148 MB model and build output out of git.
