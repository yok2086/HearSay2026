# HearSay tutorial mode

Runs on the laptop. For each action it plays a short demo video, lets the user
try it, and checks the result live from the Ultra96:

| Step | How it is checked |
|---|---|
| Glove calibration | both gloves report calibrated (`sys/<glove>/...`) |
| VR navigation, choosing a role, live captions | the tester presses **Done** |
| Signs (default: hello, yes, no, help, please, thank you) | gesture result on `tele/glove_pair/result`; pass = 3 correct out of 5 |
| Sound direction | mic board reports LEFT, then RIGHT (`tele/+/raw`) |
| Summary | accuracy per sign + overall, and which features were tried |

## Run it

```bash
cd ~/HearSay2022/Comms/tutorial
pip install paho-mqtt            # once
# open the MQTT tunnel to the Ultra96 first (same as for dashboard.py), then:
python3 tutorial.py
# open http://localhost:8000 in Chrome / Safari, full screen
```

Without the Ultra96 (to rehearse, or to check the videos):

```bash
python3 tutorial.py --demo
```

Keys in demo mode: **C** correct sign, **W** wrong sign, **L / R** sound direction,
**K** gloves calibrated. Always: **Space** = Try, **← / →** = back / next.

Other options:

```bash
python3 tutorial.py --signs all                      # practise all 20 signs
python3 tutorial.py --signs hello,thank_you,help     # pick your own
python3 tutorial.py --user nathan --password mqtt --ca ../mqtt/mosquitto_certs/ca.crt
python3 tutorial.py --insecure                       # if TLS complains about the host name
```

## Adding the demo videos

Put one clip per step in `videos/`, named after the step:

```
videos/calibration.mp4    videos/vr_navigation.mp4    videos/roles.mp4
videos/hello.mp4          videos/yes.mp4              videos/no.mp4
videos/help.mp4           videos/please.mp4           videos/thank_you.mp4
videos/direction.mp4      videos/captions.mp4         (welcome.mp4 optional)
```

Sign names must match the model's labels: hello, see_you_later, me, yes, no, help,
please, thank_you, want, what, again, eat_food, more, go_to, toilet, fine, like,
learn, sign, finish. `.mp4`, `.webm`, `.mov` and `.gif` all work; missing videos
show a placeholder.

To shrink a phone recording (trim to 3 s starting at 0.5 s, 640 px wide, no sound):

```bash
ffmpeg -i hello_raw.mov -ss 0.5 -t 3 -vf "scale=640:-2" -an -c:v libx264 -crf 28 -pix_fmt yuv420p videos/hello.mp4
```

Pass mark and tries per sign: `PASS_NEEDED` and `MAX_TRIES` at the top of `tutorial.py`.
