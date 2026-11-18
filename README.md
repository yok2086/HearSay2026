# Running the HearSay pipeline

How to run the full pipeline from a terminal after pulling `main`:
gloves and headset → SSH via stujump → Mosquitto on the Ultra96 → TCN on the
DPU → phones.

```
Glove L / Glove R / Headset (ESP32)
        │  MQTT over an SSH tunnel (ESP32 → stujump → Ultra96 localhost:1883)
        ▼
Ultra96: Mosquitto broker ──► tcn_handler.py (DPU) ──► tele/glove_pair/result
        ▲                                                   │
        │  Mac: SSH tunnel to localhost:8883 (TLS)          ▼
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
| The handler log says `No Devices Found` | `XILINX_XRT` isn't set. The handler sets it to `/usr` itself, so make sure the Ultra96 copy is up to date. |
| A glove never appears in the coordinator | Check that its `DEVICE_ID` is `glove_01L` or `glove_01R`, that `Secrets.h` is correct, and the Serial Monitor for WiFi or SSH errors. |
| A glove shows STALE or OFFLINE | Nothing heard for 10 s, or a broker disconnect. The glove reconnects on its own, and the Serial Monitor shows why it dropped. |
| A glove reboots during calibration | The vibration motor is pulling the supply down. Drive it through a transistor with a flyback diode. |
| Many `[align]` lines and poor recognition | One glove is stalling, usually its power supply on LiPo. Test it on USB-C to compare. |
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

The phone app is the Unity project in `HearSay2022/`. It installs on the phone
as **HearSay Sherpa**. Speech captions are made on the phone with sherpa-onnx,
so words appear while the person is still speaking. ASL translations and sound
direction come from the pipeline above.

```
Lavalier mic ──► Phone: noise gate ──► sherpa-onnx ──► captions
Phone camera ──► MediaPipe (face / body) ──► speaker highlight
Ultra96 broker ──► tele/glove_pair/result ──► ASL text
               ──► tele/mic_lr_01/raw     ──► left/right sound waves
```

### One-time setup

- **Unity:** install Unity **2023.2.22f1** with Android Build Support, and open
  the `HearSay2022/` folder in Unity Hub.
- **Phone:** turn on **Developer options → USB debugging**. When the phone is
  plugged into the Mac, tap **Allow** on the "Allow USB debugging?" prompt.
  Check the Mac can see it:
  ```bash
  adb devices
  ```
- **Lavalier mic:** a wireless lavalier whose receiver plugs into the phone.
  Android switches to it automatically.
- **Secrets:** the app needs `UnitySecrets.cs` (see section 1, Secrets).

### Settings

Change these in
`HearSay2022/Assets/MediaPipeUnity/Samples/Common/Scripts/HearSaySherpa.cs` if
needed:

| Setting | Current value | What it does |
|---|---|---|
| `AutoGate` | `true` | Sets the noise gate from the measured room noise |
| `NoiseGate` | `0.025` | Lowest gate level (fixed level if `AutoGate` is off) |
| `GateAboveNoise` | `3` | Gate sits this many times above the room noise |
| `GateMax` | `0.2` | Highest gate level |
| `ShowMicMeter` | `true` | Shows the mic meter on the caption screen |
| `Fixes` | `hullo → hello`, … | Words that sherpa often gets wrong |

---

## 8. Building and installing the app

1. Plug the phone into the Mac.
2. In Unity, click **Assets → Refresh** and wait for compiling to finish.
3. Click **HearSay → VR → Build and run Sherpa APK on phone**.

This builds `HearSay2022/Builds/HearSay-Sherpa.apk` and installs it on the
connected phone. Rebuild and reinstall on both phones after any app change.

To install without a cable, copy `HearSay-Sherpa.apk` to the phone (e.g. Google
Drive) and open it there.

---

## 9. Running the app

1. Start the pipeline (section 3) for ASL translations and sound direction.
2. Plug the lavalier receiver into the Deaf user's phone, and clip the
   transmitter near the speaker's mouth.
3. Open **HearSay Sherpa** on both phones, put them in the VR headsets, and
   choose:
   - **Deaf user:** captions next to the speaker, speaker highlighting, and
     left/right sound waves from the headset.
   - **Conversation partner:** sees the recognised ASL words.
4. The first launch takes a few extra seconds while the speech model is unpacked.

**Selecting buttons:** look at a button for 2 seconds. On phones without a
gyroscope (Galaxy A10), tap the button on the screen instead.

---

## 10. Checking the phone logs

With the phone connected:

```bash
adb logcat -s Unity | grep --line-buffered "HearSay"
```

Useful lines:

- `HearSay caption: <text>`: a finished caption.
- `HearSay mic: lowest ... highest ... room noise ... gate ...`: printed once a
  second. `SPEECH` means the mic was above the gate, `quiet` means it was
  ignored.
- `HearSay captions: using sherpa-onnx on the phone (streaming)`: sherpa
  started correctly.

The mic meter in the top-left of the caption screen shows the same levels, e.g.
`Mic 0.012  peak 0.090  gate 0.025 auto`.

---

## 11. Phone app troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| Unity says "No Android devices connected" | The Mac can't see the phone. Run `adb devices`. If empty, use a data cable (not charge-only), another USB port, and allow USB debugging on the phone. |
| Build fails with "script class layout is incompatible" | Unity hasn't reloaded the scripts. Click on Unity, wait for compiling to finish, then build again. |
| Captions appear when nobody is talking | Background noise is above the gate. Check the mic meter, and raise `NoiseGate` or `GateAboveNoise`. |
| Quiet speech is missed or words get cut off | The gate is too high. Speak closer to the lavalier, or lower `GateAboveNoise`. |
| Words are often wrong | Use the lavalier close to the mouth. Add repeated mistakes to `Fixes`. |
| Captions lag behind speech | The phone is too slow (e.g. Galaxy A10). Use the Galaxy Z Fold4. |
| No speaker outline on the Galaxy A10 | MediaPipe needs a 64-bit phone. Captions still work. |
| No ASL words or sound waves | The pipeline isn't running, or the phone has no network. See sections 3 and 6. |
