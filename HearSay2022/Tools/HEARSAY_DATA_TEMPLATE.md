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

Allowed `soundDirection`: `left`, `right`, `front`, `back`.

Allowed `aslWord`: `hello`, `see_you_later`, `i_me`, `yes`, `no`, `help`, `please`, `thank_you`, `want`, `what`, `again_repeat`, `eat_food`, `more`, `go_to`, `bathroom`, `fine`, `like`, `learn`, `sign`, `finish_done`.

The server accepts real updates for 10 seconds after the most recent message. If updates stop, it goes back to random dummy values so the presentation still has visible input.

Use `HearSaySensorSenderTemplate.py` as the sender starting point. It uses only Python's standard library.
