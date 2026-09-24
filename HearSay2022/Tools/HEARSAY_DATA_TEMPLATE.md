# HearSay sensor-to-Unity message template

Run the server on the laptop:

```bash
python3 Tools/HearSayDummyServer.py
```

The terminal prints two URLs. Unity uses the `/status` URL. A sensor or ASL-model teammate sends a JSON `POST` to the `/update` URL:

```json
{
  "soundActive": true,
  "soundDirection": "left",
  "aslDetected": true,
  "aslWord": "hello",
  "aslConfidence": 0.91
}
```

Allowed `soundDirection`: `left`, `right`. Use `soundActive: false` for off.

Allowed `aslWord`: `hello`, `see_you_later`, `i_me`, `yes`, `no`, `help`, `please`, `thank_you`, `want`, `what`, `again_repeat`, `eat_food`, `more`, `go_to`, `bathroom`, `fine`, `like`, `learn`, `sign`, `finish_done`.

Real data expires after 10 seconds without another update. The server marks it stale and sets soundActive/aslDetected false; it does NOT replace real data with random values. The explicit terminal command `random` still enables demo data.

Use `HearSaySensorSenderTemplate.py` as the sender starting point. It uses only Python's standard library.

## Teammate phone Data Monitor

Rebuild the APK. On Home, open Connection settings and use the server's printed
/status URL. Choose **Data monitor / teammate**. No camera or microphone is started.
It displays sound direction, ASL word/confidence, source, received-response count,
raw JSON and the last 40 value changes. This is received sensor data, not arbitrary
stdout from the Ultra96. The phone polls every 0.25 seconds; this is latest-state
monitoring, not a guaranteed record of every sensor event.

Current route: Ultra96/microphone program -> laptop HTTP server -> phone.
All devices must reach the laptop on the same LAN (no isolated guest Wi-Fi).
No USB/serial/TCP-specific hardware receiver is implemented here.
If the Ultra96 already has its own API, agree on that protocol before changing Unity.

### Quick test

Start the existing server first. Replace LAPTOP_IP below with its printed address.
Open the app and let its launch reset finish BEFORE sending test data.

```bash
python3 Tools/HearSayUltra96Sender.py --server http://LAPTOP_IP:8080 --sound left --asl "hello"
python3 Tools/HearSayUltra96Sender.py --server http://LAPTOP_IP:8080 --sound right --asl "thank you"
python3 Tools/HearSayReceiverTemplate.py --server http://LAPTOP_IP:8080 --once
```

Send both inactive:

```bash
python3 Tools/HearSayUltra96Sender.py --server http://LAPTOP_IP:8080
```

The sender uses only Python's standard library and accepts all 20 word-bank items
listed above. User-readable spaces/slashes are converted to underscores:
I/Me -> i_me, again/repeat -> again_repeat, finish/done -> finish_done.
Use left/right for sound; front/back/up/down are no longer accepted.
Inactive fields keep a valid placeholder word/direction; consumers must check the
boolean flags, not display the placeholder.

### Use real model/sensor results

Copy HearSayUltra96Sender.py to the Ultra96 and import its two functions:

```python
from HearSayUltra96Sender import build_payload, send_update

# Inside your existing sensor/model loop:
# direction: "left", "right", or None
# word: one of the 20 words, or None
# confidence: 0.0..1.0
payload = build_payload(direction, word, confidence)
send_update("http://LAPTOP_IP:8080", payload)
```

Handle network errors in the caller and send a current snapshot periodically
(e.g. twice per second, including no-detection flags). Do not invent new
recognitions just to keep the connection alive.

POST /update replaces ALL five fields. If sound and ASL come from different
programs, combine their latest values in ONE sending program; independent full
snapshots would overwrite each other. POST returns {"ok": true, ...}.
GET /status returns those fields plus source and, for real data, stale/ageSeconds.
The receiving Python file is an example of the GET side; HearSayDataMonitor.cs is
the actual Unity receiving implementation.

Important: the existing app-launch POST /reset clears shared sound/ASL values on
the server for BOTH phones. Launch phones before starting the sender. Reset
returns to manual mode; the next real POST /update resumes real input.
These endpoints have no authentication: use a trusted LAN, not public Internet.
