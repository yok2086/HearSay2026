"""
Glove serial logger: connect one or two gloves, calibrate flex sensors,
record labelled signs.

Run in Windows PowerShell (not WSL), with the Arduino Serial Monitor closed
(only one program can hold a COM port):
  pip install pyserial
  python logger.py p01_session1 --left COM4 --right COM6   # two gloves
  python logger.py p01_session1 --right COM6                # one glove

Each glove has its own COM port, also through a USB hub. Plug each glove into
the same hub slot every time so its COM number stays the same.

Output:
  data/<session>/
  ├── calib.json     {"left": {"straight": [5], "bent": [5]}, "right": {...}, ...}
  ├── hello-01.csv   one row per frame: L_millis,L_ax,...,L_f5,R_millis,R_ax,...,R_f5
  └── ...            (one-glove sessions only have that hand's columns)
Flex order: thumb, index, middle, ring, pinky.

Two-hand alignment, per take:
  1. Each hand must be continuous: ~100 Hz, no gap over MAX_GAP_MS.
  2. Both hands' samples are put on the laptop clock, leading samples are
     dropped from whichever hand started earlier, and both are cut to the same
     length. The first and last frames of the two hands must then be within
     MAX_OFFSET_MS of each other.
  If either check fails, saving defaults to No.

One session = one time putting the gloves on. Took them off? Start a new session.
"""
import os
import re
import sys
import json
import time
import argparse
import threading
import serial

BAUD = 921600
N_FIELDS = 12          # millis, ax, ay, az, gx, gy, gz, f1..f5
CALIB_SECONDS = 1.0
SIDE_CHECK_SECONDS = 2.0
MIN_RANGE = 100        # min |bent - straight| in ADC counts; below = loose/broken sensor (tune after first tries)
MIN_SAMPLES = 30       # warn if a recording is shorter than this (~0.3 s)
MAX_GAP_MS = 25        # warn if two samples of one hand are further apart than this
MAX_OFFSET_MS = 20     # warn if the hands' first or last frames differ by more than this (2 frames)
FINGERS = ["thumb", "index", "middle", "ring", "pinky"]
FIELDS = ["millis", "ax", "ay", "az", "gx", "gy", "gz", "f1", "f2", "f3", "f4", "f5"]
PREFIX = {"left": "L", "right": "R"}


# ---------------------------------------------------------------- serial

def parse_line(line):
    """(millis, [6 imu], [5 flex]) for a valid data line, else None."""
    if not line or line.startswith("#"):
        return None
    parts = line.split(",")
    if len(parts) != N_FIELDS:
        return None
    try:
        return int(parts[0]), [float(v) for v in parts[1:7]], [int(v) for v in parts[7:12]]
    except ValueError:
        return None


def open_port(port):
    """Open without toggling DTR/RTS (avoids the ESP32 reset on Windows),
    then wait for the first valid data line in case it reset anyway."""
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = BAUD
    ser.timeout = 1
    ser.dtr = False
    ser.rts = False
    try:
        ser.open()
    except serial.SerialException as e:
        sys.exit(f"could not open {port}: {e}\n(is the Arduino Serial Monitor still open?)")

    deadline = time.time() + 5
    while time.time() < deadline:
        line = ser.readline().decode(errors="ignore").strip()
        if line.startswith("#"):
            print(f"  {port}: {line}")
        elif parse_line(line):
            return ser
    ser.close()
    sys.exit(f"no data from {port} - check port and baud rate")


class Reader(threading.Thread):
    """Reads one glove's port continuously in the background, so recording keeps
    going while the main thread waits for Enter. Keeps lines only while capturing,
    each with the laptop arrival time in ms."""

    def __init__(self, ser, hand):
        super().__init__(daemon=True)
        self.ser = ser
        self.hand = hand
        self.lock = threading.Lock()
        self.capturing = False
        self.buf = []          # (arrival_ms, raw line, parsed sample)
        self.bad = 0           # malformed lines dropped while capturing
        self.error = None

    def run(self):
        while self.error is None:
            try:
                raw = self.ser.readline()
            except serial.SerialException as e:
                self.error = str(e)
                break
            t = time.perf_counter() * 1000
            line = raw.decode(errors="ignore").strip()
            if line.startswith("#"):
                print(f"\n  {self.hand} glove: {line}")
                continue
            sample = parse_line(line)
            with self.lock:
                if not self.capturing:
                    continue
                if sample is None:
                    if line:
                        self.bad += 1
                    continue
                self.buf.append((t, line, sample))

    def start_capture(self):
        with self.lock:
            self.buf, self.bad, self.capturing = [], 0, True

    def stop_capture(self):
        with self.lock:
            self.capturing = False
            return self.buf, self.bad

    def check(self):
        if self.error:
            sys.exit(f"serial error on {self.hand} glove: {self.error} (unplugged?)")


def capture(readers, seconds=None):
    """Capture from all gloves at once, for `seconds` or until Enter.
    Returns {hand: (samples, dropped_lines)}."""
    for r in readers.values():
        r.check()
        r.start_capture()
    if seconds is None:
        input("  RECORDING ... press Enter to stop")
    else:
        time.sleep(seconds)
    caps = {h: r.stop_capture() for h, r in readers.items()}
    for r in readers.values():
        r.check()
    return caps


# ---------------------------------------------------------------- port check

def check_sides(readers):
    """Catch swapped ports: wave only the left hand and see which port moves."""
    if len(readers) < 2:
        return readers
    while True:
        input("\nSide check: keep your RIGHT hand still, press Enter, then rotate your LEFT hand for ~2 s...")
        caps = capture(readers, SIDE_CHECK_SECONDS)
        motion = {}
        for hand, (buf, _) in caps.items():
            if not buf:
                sys.exit(f"no samples from {hand} glove")
            motion[hand] = sum(sum(abs(g) for g in s[1][3:6]) for _, _, s in buf) / len(buf)
        print(f"  rotation: --left port {motion['left']:.0f} deg/s, --right port {motion['right']:.0f} deg/s")

        if motion["left"] > 2 * motion["right"]:
            print("  OK: ports match the hands")
            return readers
        if motion["right"] > 2 * motion["left"]:
            if input("  The ports look SWAPPED. Swap them for this session? [Y/n] ").strip().lower() != "n":
                left, right = readers["right"], readers["left"]
                left.hand, right.hand = "left", "right"
                print(f"  swapped (next time run with --left {left.ser.port} --right {right.ser.port})")
                return {"left": left, "right": right}
            continue
        print("  unclear: move the left hand more and keep the right hand still")


# ---------------------------------------------------------------- calibration

def average_flex(readers, seconds):
    out = {}
    for hand, (buf, _) in capture(readers, seconds).items():
        if not buf:
            sys.exit(f"no samples from {hand} glove during calibration")
        out[hand] = [sum(s[2][i] for _, _, s in buf) / len(buf) for i in range(5)]
    return out


def calibrate(readers):
    while True:
        input("\nHold fingers STRAIGHT on every glove, press Enter, keep still ~1 s...")
        straight = average_flex(readers, CALIB_SECONDS)
        input("Make a FIST with every glove, press Enter, keep still ~1 s...")
        bent = average_flex(readers, CALIB_SECONDS)

        weak = []
        for hand in readers:
            print(f"\n  {hand} glove")
            print(f"  {'finger':7s} {'straight':>9s} {'bent':>9s} {'range':>7s}")
            for name, s, b in zip(FINGERS, straight[hand], bent[hand]):
                r = b - s
                flag = ""
                if abs(r) < MIN_RANGE:          # abs: direction depends on divider wiring
                    weak.append(f"{hand} {name}")
                    flag = "  <-- too small"
                print(f"  {name:7s} {s:9.1f} {b:9.1f} {r:7.1f}{flag}")

        if not weak:
            return straight, bent
        print(f"\nWARNING: small range for {', '.join(weak)} (sensor loose or broken?)")
        if input("Redo calibration? [Y/n] ").strip().lower() == "n":
            return straight, bent


def load_or_calibrate(readers, out_dir):
    path = os.path.join(out_dir, "calib.json")
    if os.path.exists(path):
        with open(path) as f:
            old = json.load(f)
        if all(h in old for h in readers):
            if input(f"{path} exists. Reuse it (same gloves-on session)? [y/N] ").strip().lower() == "y":
                return
        else:
            print(f"{path} doesn't cover {' and '.join(readers)} glove(s), so calibrating again")
    straight, bent = calibrate(readers)
    calib = {h: {"straight": [round(v, 1) for v in straight[h]],
                 "bent": [round(v, 1) for v in bent[h]]} for h in readers}
    calib["fingers"] = FINGERS
    calib["time"] = time.strftime("%Y-%m-%d %H:%M:%S")
    with open(path, "w") as f:
        json.dump(calib, f, indent=2)
    print(f"saved {path}")


# ---------------------------------------------------------------- checks + alignment

def check_hand(hand, buf, bad):
    """Check 1: one hand's recording is long enough and continuous."""
    n = len(buf)
    if n < 2:
        print(f"  {hand:5s}: only {n} samples")
        return False
    t = [s[0] for _, _, s in buf]
    dur = (t[-1] - t[0]) / 1000
    gaps = [b - a for a, b in zip(t, t[1:])]
    rate = (n - 1) / dur if dur > 0 else 0
    print(f"  {hand:5s}: {n} samples, {dur:.2f} s, {rate:.1f} Hz, "
          f"max gap {max(gaps)} ms, dropped lines {bad}")
    ok = True
    if n < MIN_SAMPLES:
        print(f"  WARNING: very short recording ({hand})")
        ok = False
    if max(gaps) > MAX_GAP_MS or min(gaps) <= 0:
        print(f"  WARNING: gap or timestamp jump on {hand} glove (board reset?)")
        ok = False
    return ok


def laptop_times(buf):
    """Each sample's time on the laptop clock. USB can only delay a line, never
    deliver it early, so the smallest (arrival - millis) is the best estimate of
    the offset between the two clocks. The spacing then comes from the glove's
    own clock, without USB jitter."""
    offset = min(t - s[0] for t, _, s in buf)
    return [s[0] + offset for _, _, s in buf]


def align(left, right):
    """Check 2: drop leading samples from whichever hand started earlier, cut both
    to the same length, and return the first/last frame offsets (ms, right - left)."""
    tl, tr = laptop_times(left), laptop_times(right)
    i = j = 0
    while i + 1 < len(tl) and abs(tl[i + 1] - tr[0]) < abs(tl[i] - tr[0]):
        i += 1
    while j + 1 < len(tr) and abs(tr[j + 1] - tl[i]) < abs(tr[j] - tl[i]):
        j += 1
    n = min(len(tl) - i, len(tr) - j)
    if n < 1:
        return [], [], float("inf"), float("inf")
    start = tr[j] - tl[i]
    end = tr[j + n - 1] - tl[i + n - 1]
    return left[i:i + n], right[j:j + n], start, end


def check_take(caps):
    """Run both checks. Returns (looks_fine, rows), rows = list of raw-line tuples."""
    ok = True
    for hand, (buf, bad) in caps.items():
        ok = check_hand(hand, buf, bad) and ok

    if len(caps) == 1:
        (buf, _), = caps.values()
        return ok, [(line,) for _, line, _ in buf]

    left, right = caps["left"][0], caps["right"][0]
    if len(left) < 2 or len(right) < 2:
        return False, []
    l, r, start, end = align(left, right)
    print(f"  align: {len(l)} frames, start offset {start:+.1f} ms, end offset {end:+.1f} ms, "
          f"trimmed {len(left) - len(l)} left / {len(right) - len(r)} right samples")
    if abs(start) > MAX_OFFSET_MS or abs(end) > MAX_OFFSET_MS:
        print("  WARNING: hands don't line up (gap on one glove, or a late start)")
        ok = False
    return ok, [(a[1], b[1]) for a, b in zip(l, r)]


# ---------------------------------------------------------------- recording

def next_filename(out_dir, sign):
    """hello-01.csv, hello-02.csv, ... continuing from files already there."""
    pat = re.compile(rf"^{re.escape(sign)}-(\d+)\.csv$")
    nums = [int(m.group(1)) for f in os.listdir(out_dir) if (m := pat.match(f))]
    return f"{sign}-{max(nums, default=0) + 1:02d}.csv"


def record_loop(readers, out_dir):
    header = ",".join(f"{PREFIX[h]}_{f}" for h in readers for f in FIELDS)
    print("\nType a sign name and press Enter to START. Press Enter again to STOP.")
    print("Empty name = same sign as last time. 'q' = quit.")
    last = None
    while True:
        prompt = f"\nsign [{last}]: " if last else "\nsign: "
        name = input(prompt).strip().lower()
        if name == "q":
            return
        if not name:
            if not last:
                continue
            name = last
        if not re.fullmatch(r"[a-z0-9_]+", name):
            print("  use only a-z, 0-9 and _ (no spaces or dashes)")
            continue

        print(f"  '{name}'")
        fine, rows = check_take(capture(readers))
        if not rows:
            print("  nothing to save - discarded")
            last = name
            continue
        fname = next_filename(out_dir, name)
        default = "Y/n" if fine else "y/N"
        ans = input(f"  save as {fname}? [{default}] ").strip().lower()
        if ans == "y" or (ans == "" and fine):
            with open(os.path.join(out_dir, fname), "w", newline="") as f:
                f.write(header + "\n")
                f.writelines(",".join(row) + "\n" for row in rows)
            print(f"  saved {fname}")
        else:
            print("  discarded")
        last = name


# ---------------------------------------------------------------- main

def main():
    p = argparse.ArgumentParser()
    p.add_argument("session", help="e.g. p01_session1")
    p.add_argument("--left", help="COM port of the left glove, e.g. COM4")
    p.add_argument("--right", help="COM port of the right glove, e.g. COM6")
    args = p.parse_args()
    if not (args.left or args.right):
        p.error("give --left and/or --right")
    if args.left and args.right and args.left.upper() == args.right.upper():
        p.error("--left and --right must be different ports")

    out_dir = os.path.join("data", args.session)
    os.makedirs(out_dir, exist_ok=True)

    readers = {}
    try:
        for hand, port in (("left", args.left), ("right", args.right)):
            if port:
                ser = open_port(port)
                print(f"{hand} glove connected on {port} at {BAUD}")
                readers[hand] = Reader(ser, hand)
                readers[hand].start()

        readers = check_sides(readers)
        load_or_calibrate(readers, out_dir)
        record_loop(readers, out_dir)
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        for r in readers.values():
            r.ser.close()
        print("port(s) closed")


if __name__ == "__main__":
    main()
