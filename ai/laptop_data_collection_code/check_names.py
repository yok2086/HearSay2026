"""
Check that every recording in data/<session>/ is named after a known sign.

Run from the folder that contains data/ (the logger folder):
    python check_names.py

A file like again-01.csv counts as the sign "again". Prints, per session:
  - files whose sign isn't in SIGNS
  - files that don't follow the <sign>-<number>.csv pattern
  - signs from SIGNS with no recordings in that session
and then a table of takes per sign per session.
"""
import os
import re
import sys

DATA_DIR = "data"
SIGNS = [
    "hello", "see_you_later", "me", "yes", "no", "help", "please", "thank_you",
    "want", "what", "again", "eat_food", "more", "go_to", "toilet", "fine",
    "like", "learn", "sign", "finish",
]
NAME = re.compile(r"^(.+)-(\d+)\.csv$")

if not os.path.isdir(DATA_DIR):
    sys.exit(f"no '{DATA_DIR}' folder here - run this from the logger folder")

sessions = sorted(d for d in os.listdir(DATA_DIR) if os.path.isdir(os.path.join(DATA_DIR, d)))
print(f"{len(sessions)} session folder(s): {', '.join(sessions)}")

counts = {}          # (session, sign) -> number of takes
problems = 0
for session in sessions:
    files = sorted(f for f in os.listdir(os.path.join(DATA_DIR, session)) if f.lower().endswith(".csv"))
    unknown, bad_format = [], []
    for f in files:
        m = NAME.match(f)
        if not m:
            bad_format.append(f)
        elif m.group(1) not in SIGNS:
            unknown.append(f)
        else:
            counts[(session, m.group(1))] = counts.get((session, m.group(1)), 0) + 1
    missing = [s for s in SIGNS if (session, s) not in counts]

    print(f"\n{session}: {len(files)} csv files")
    if unknown:
        print(f"  unknown sign name: {', '.join(unknown)}")
    if bad_format:
        print(f"  not <sign>-<number>.csv: {', '.join(bad_format)}")
    if missing:
        print(f"  no recordings for: {', '.join(missing)}")
    if not (unknown or bad_format or missing):
        print("  OK")
    problems += len(unknown) + len(bad_format) + len(missing)

# takes per sign per session
w = max(len(s) for s in SIGNS)
print(f"\n{'sign':{w}s} " + " ".join(f"{s[:10]:>10s}" for s in sessions) + "   total")
for sign in SIGNS:
    row = [counts.get((s, sign), 0) for s in sessions]
    print(f"{sign:{w}s} " + " ".join(f"{n:10d}" for n in row) + f"   {sum(row):5d}")

print("\nall files OK" if problems == 0 else f"\n{problems} problem(s) found")
