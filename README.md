# Running the HearSay pipeline

How to run the full pipeline from a terminal after pulling `main`:
gloves and headset → SSH via stujump → Mosquitto on the Ultra96 → TCN on the
DPU → phones.

```
Glove L / Glove R / Headset (ESP32)
        │  
        ▼
Ultra96: Mosquitto broker ──► tcn_handler.py (DPU) ──► tele/glove_pair/result
        ▲                                                   │
        │                                                   ▼
Mac: coordinator / dashboard / stats               Phones (Unity app)
```

`run_hearsay_pipeline.sh` starts everything on the Ultra96 and the Mac in one go.

---

## 1. One-time setup

### Mac

- **Tools:** `tmux`, `lsof`, an SSH client, and Python 3 with `paho-mqtt`:
  ```bash
  brew install tmux
  ~/miniconda3/bin/python3 -m pip install paho-mqtt
  ```
- **SSH access:** you need logins for `stujump.comp.nus.edu.sg` and the
  Ultra96 (`xilinx@makerslab-fpga-36.ddns.comp.nus.edu.sg`). Add your public
  key to both so the script doesn't keep asking for passwords. Check it works:
  ```bash
  ssh -J <you>@stujump.comp.nus.edu.sg xilinx@makerslab-fpga-36.ddns.comp.nus.edu.sg 'echo ok'
  ```
- **Network:** stujump SSH is blocked from some campus networks. If the command
  above times out, connect to the NUS VPN first.

### Paths and usernames hard-coded for the original machine

Change these to match your machine:

| File | Setting | Current value |
|---|---|---|
| `run_hearsay_pipeline.sh` | `JUMP_HOST` | `nathan@stujump.comp.nus.edu.sg` |
| `run_hearsay_pipeline.sh` | `LOCAL_DIR` | `$HOME/HearSay2026/Comms` |
| `run_hearsay_pipeline.sh` | `PYTHON_BIN` | `$HOME/miniconda3/bin/python3` |
| `coordinator.py`, `dashboard.py`, `stats.py` | `CA_CERT_PATH` | `/Users/NathanTan_1/HearSay2026/Comms/mqtt/mosquitto_certs/ca.crt` |

### Ultra96

The script expects these files to already be on the Ultra96:

| Path on the Ultra96 | What it is | Source |
|---|---|---|
| `/home/xilinx/mqtt/mosquitto.conf` | Broker config (8883 TLS for the Mac, 1883 for the ESP32s, both localhost only) | `Comms/mqtt/mosquitto.conf` |
| `/home/xilinx/mqtt/mosquitto_passwd` | Broker logins | `Comms/mqtt/mosquitto_passwd` |
| `/home/xilinx/mqtt/mosquitto_certs/` | `ca.crt`, `server.crt`, `server.key` | `Comms/mqtt/mosquitto_certs/` |
| `/home/xilinx/jupyter_notebooks/tcn/tcn_handler.py` | Inference bridge | `Comms/ultra96_scripts/tcn_handler.py` |
| `/home/xilinx/jupyter_notebooks/tcn/tcn_infer.py` | DPU wrapper | `Comms/tcn_infer.py` |
| `/home/xilinx/jupyter_notebooks/tcn/tcn.xmodel`, `dpu.bit` (and its `.hwh`/`.xclbin`), `norm_stats.npz` | Model and DPU overlay | **Not in the repo.** Get them from the ML side. |

Also needed on the Ultra96:

- **Mosquitto:** check with `which mosquitto`.
- **`paho-mqtt` in the PYNQ venv**, which the handler runs under as root:
  ```bash
  sudo /usr/local/share/pynq-venv/bin/python3 -m pip install paho-mqtt
  ```
- **Passwordless sudo for that Python.** The script runs it with `sudo -n`, so
  without this the handler silently fails to start. Run `sudo visudo` and add:
  ```
  xilinx ALL=(root) NOPASSWD: /usr/local/share/pynq-venv/bin/python3, /usr/bin/pkill
  ```

### Secrets (not in the repo)

Get these from a teammate. Never commit them:

- **Firmware:** each sketch folder (`firebeetle_glove/`, `CG4002_AR_headset/`, …)
  needs a `Secrets.h` with `WIFI_SSID`, `EAP_IDENTITY`, `EAP_USERNAME`,
  `EAP_PASSWORD`, `SOC_USERNAME`, `SOC_PASSWORD` and `SSH_PRIVATE_KEY`.
- **App:** `HearSay2022/Assets/MediaPipeUnity/Samples/Common/Scripts/UnitySecrets.cs`.

---

## 2. Updating Ultra-96 code

Copy over any Ultra96-side code that changed. From the repo root:

```bash
J=<you>@stujump.comp.nus.edu.sg
U=xilinx@makerslab-fpga-36.ddns.comp.nus.edu.sg
scp -J $J Comms/ultra96_scripts/tcn_handler.py Comms/tcn_infer.py $U:/home/xilinx/jupyter_notebooks/tcn/
scp -J $J Comms/mqtt/mosquitto.conf $U:/home/xilinx/mqtt/
```

Then reflash and rebuild as needed:

- **Firmware changes:** reflash the ESP32s from the Arduino IDE.
  - `firebeetle_glove.ino` is shared by both gloves. Set
    `DEVICE_ID = "glove_01L"` before flashing the left glove and `"glove_01R"`
    before flashing the right one.
  - The headset sketch is `CG4002_AR_headset.ino`, with
    `DEVICE_ID = "mic_lr_01"`.
- **App changes:** rebuild the Unity app and reinstall it on both phones.

---

## 3. Run it

```bash
cd ~/HearSay2026
./Comms/run_hearsay_pipeline.sh
```

The script:

1. Starts Mosquitto on the Ultra96, or reuses it if it's already running.
2. Starts `tcn_handler.py` on the DPU, or reuses it if it's already running.
3. Opens a background tunnel from Mac port 8883 to the Ultra96 broker. It
   listens on `127.0.0.1` only, so only programs on the Mac can use it.
4. Opens a `tmux` session `hearsay` with three windows:
   - `coordinator`: device states (RUNNING, STALE, OFFLINE)
   - `dashboard`: live messages, including calibration progress
   - `stats`: message rate and latency

When it prints **"Now power on the FireBeetle"**:

1. Power on both gloves and the headset. Each one connects through stujump
   (about 5–10 s) and publishes `HELLO`. Check that `glove_01L`, `glove_01R`
   and `mic_lr_01` appear in the `coordinator` window.
2. Calibrate each glove when its vibration motor cues you. Watch for
   `CALIBRATION_STATUS` in the `dashboard` window:
   - **Straight:** keep your fingers straight and **hold the hand completely
     still**. The gyro offset is measured at this step, and moving gives a bad
     offset for the whole session.
   - **Fist:** make a fist when prompted.
3. Open **HearSay Sherpa** on both phones (see section 7), choosing:
   - **Conversation partner:** sees the recognised words, and the headset
     speaker reads them aloud.
   - **Deaf user:** sees left/right sound waves from the headset, plus captions.
4. Sign. Recognised words appear on the partner's phone and are spoken by the
   headset.

**Switching tmux windows:** `Ctrl+B` then the window number, or `Ctrl+B` then
`n` for the next window.

---

## 4. Stop it

1. Detach from tmux with **`Ctrl+B` then `D`**. The script then cleans up
   automatically: it stops the broker and handler on the Ultra96, closes the
   tunnel, and kills the tmux session.
2. Power off the gloves and headset.

`Ctrl+C` inside a tmux window only stops that one program. Use detach to stop
everything.

---

## 5. Checking the logs

On the Ultra96:

```bash
ssh -J <you>@stujump.comp.nus.edu.sg xilinx@makerslab-fpga-36.ddns.comp.nus.edu.sg
tail -f ~/logs/ultra96_inference.log   # handler: calibration, windows, published words
tail -f ~/logs/mosquitto.log           # broker connections
```

Useful lines in the handler log:

- `[calibration] glove_01L: ... gyro_offset=[...]`: should be within a few deg/s
  of 0. If any value is large, recalibrate and keep the hand still.
- `[warn] ... small flex range for finger(s) [...]`: that flex sensor barely
  moved, so check its wiring.
- `[align] dropped N unpaired samples, filled M missing frames`: occasional
  small numbers are normal. A steady stream means a glove is stalling.
- `[inference] <word> (<conf>) -> tele/glove_pair/result`: a published word.

To log every sample's timestamp for timing analysis, set `PRINT_SAMPLES = True`
in `tcn_handler.py`. That's about 200 lines per second.

For a glove's own view, open the Arduino Serial Monitor at 115200 baud. The
status line shows `sent`, `buffered`, `dropped` and `pub ok`/`fail`.

---

## 6. Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| The script hangs or times out at step 1 | stujump is unreachable from this network. Connect to the NUS VPN. |
| No words appear and the handler log is empty or missing | `sudo -n` failed, or `paho-mqtt` or the model files are missing on the Ultra96. Check `~/logs/ultra96_inference.log` and the one-time setup above. |
| A glove never appears in the coordinator | Check that its `DEVICE_ID` is `glove_01L` or `glove_01R`, that `Secrets.h` is correct, and the Serial Monitor for WiFi or SSH errors. |
| A glove shows STALE or OFFLINE | Nothing heard for 10 s, or a broker disconnect. The glove reconnects on its own. Connect glove and check serial monitor |
| A glove reboots during calibration | Battery is low, charge and retest |
| Many `[align]` lines and poor recognition | One glove is stalling, power supply might be weak. Charge battery and retest |
| Phones show nothing | Check both phones have network access and the app was rebuilt with the current `UnitySecrets.cs`. |
| `localhost:8883` connection refused on the Mac | The tunnel isn't up. Rerun the script, or check `lsof -i :8883`. |

### Other scripts

To use another inference script, change the commented
`REMOTE_INFERENCE_SCRIPT` / `INFERENCE_RUNNING` pairs in
`run_hearsay_pipeline.sh` (or `$RemoteInferenceScript` / `$InferenceMatch` in
`run_hearsay_pipeline.ps1`). The options are dummy inference
(`dummy_inference.py`), dummy phone sender (`dummy_phone_sender.py`) and the
older sensor inference (`ultra96_inference.py`), all in
`Comms/ultra96_scripts/`.

---

## 7. Phone app (HearSay Sherpa)

The phone app is the Unity project in `HearSay2022/` (Unity 2023.2.22f1). Speech
captions run on the phone with sherpa-onnx, so no server is needed for speech.

### Build and install

1. Plug the phone into the Mac with USB debugging on (check with `adb devices`).
2. In Unity, click **Assets → Refresh**, then
   **HearSay → VR → Build and run Sherpa APK on phone**.

This builds `HearSay2022/Builds/HearSay-Sherpa.apk` and installs it on the phone.

### Run it

1. Start the pipeline (section 3) for ASL translations and sound direction.
2. Plug the lavalier mic receiver into the Deaf user's phone.
3. Open **HearSay Sherpa** on both phones and choose **Deaf user** or
   **Conversation partner**.

To see the captions on the Mac:
```bash
adb logcat -s Unity | grep --line-buffered "HearSay"
```

---

## 8. AI model (sign classifier)

A small 1-D CNN (a temporal convolutional network, TCN) classifies 2.56 s windows
of glove data (22 channels: per hand, 3-axis accel, 3-axis gyro and 5 flex sensors,
at 100 Hz) into 20 signs. It is quantized to INT8 and runs on the Ultra96's DPU
(DPUCZDX8G B1600) in about 0.37 ms per window.

### What's in `ai/`

| Folder | Contents |
|---|---|
| `laptop_data_collection_code/` | `logger.py` (records labelled takes from both gloves over USB serial), `check_names.py` (checks take file names), `firebeetle_glove/` (the serial data-collection firmware, not the MQTT firmware used live) |
| `training_data/` | One folder per recording session: `calib.json` plus one CSV per take (`<sign>-NN.csv`) |
| `laptop_training_code/` | Dataset loading, model, training, evaluation, INT8 quantization, and `arch_ultra96.json` for the compiler |
| `ultra96_code/` | Deployed model (`tcn.xmodel`, `norm_stats.npz`, `test_data.npz`), `tcn_infer.py` (DPU wrapper), `tcn_handler.py` (live MQTT inference), `tcn_dpu.py` (accuracy and latency check), `power_test.py` |

`tcn.xmodel`, `norm_stats.npz` and `test_data.npz` all come from the same training
run and must be copied to the board together. The DPU overlay (`dpu.bit`, `.hwh`,
`.xclbin`) is not in the repo; it comes with the `pynq-dpu` 2.5.1 package installed
on the Ultra96.

### Collecting data (Windows)

```powershell
python -m serial.tools.list_ports -v      # use the two "USB-SERIAL CH340" ports
python logger.py <name>_<date> --left COMx --right COMy
```

The logger runs a side check (rotate the left hand only), then calibration (hands
flat, then fists), then recording. Close the Arduino Serial Monitor first. Copy each
new session folder into `ai/training_data/`, then run `check_names.py` and
`session_stats.py`.

### Training and deploying (Vitis AI 2.5 Docker container, CPU)

Run these from `ai/laptop_training_code/` inside the `vitis-ai-pytorch` environment.
The validation and test sessions are set by `VAL_SESSIONS` and `TEST_SESSIONS` in
`dataset.py`; every other session folder in `training_data/` is used for training.

```bash
python dataset.py                 # checks the data and the split
python train.py                   # evaluation run: saves the best-validation model
python evaluate.py                # test accuracy and misclassifications
python train.py --final           # deployment run: trains on all sessions
python quantize.py --mode calib
python quantize.py --mode test
vai_c_xir -x quantize_result/TCN_int.xmodel -a arch_ultra96.json -o compiled -n tcn
python -c "from dataset import load_all; import numpy as np; X, y = load_all()['test']; np.savez('compiled/test_data.npz', X=X, y=y)"
```

- Quote accuracy from the evaluation run. After `--final`, the test session is in the
  training data, so its scores are no longer a real test.
- Check that the compiler output says `DPU subgraph number 1`. It exits with code 0
  even when compilation fails.
- Only `train.py` writes `norm_stats.npz`. Deploy the one from the `--final` run.

Copy `compiled/tcn.xmodel`, `compiled/test_data.npz` and `norm_stats.npz` to
`/home/xilinx/jupyter_notebooks/tcn/` on the Ultra96, then check them as root with
the XRT and PYNQ environments sourced:

```bash
python3 tcn_dpu.py      # DPU accuracy on test_data.npz and latency
```

### Notes

- Sessions with faulty sensors are left out of `training_data/`, because every
  folder there is used for training.
- The flex calibration and normalisation in `tcn_handler.py` must match
  `dataset.py`. If preprocessing changes in one, change the other.
- `power_test.py` measures board power in four states (DPU idle, DPU running,
  CPU at minimum clock, one CPU core). It restores all settings at the end, but
  check `who` first, because it affects the whole board.