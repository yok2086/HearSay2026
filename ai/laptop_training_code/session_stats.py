"""
Compare channel statistics across sessions, to spot a session that looks
different from the rest (glove refitted, sensor loose, IMU mounted differently).

Run (in the container, from tcn_project):
    python session_stats.py

For every channel, prints each session's mean, using the same calibrated values
the model sees (flex normalised with calib.json, IMU raw). A '*' marks a mean
more than 1 std (pooled over the training sessions) away from the training mean.
"""
import os
import glob
import numpy as np

from dataset import (DATA_DIR, HANDS, PREFIX, IMU, FLEX, VAL_SESSIONS, TEST_SESSIONS,
                     load_calib, load_csv, remove_gyro_bias)

CH_NAMES = [f"{PREFIX[h]}_{c}" for h in HANDS for c in IMU + FLEX]

sessions = sorted(d for d in os.listdir(DATA_DIR) if os.path.isdir(os.path.join(DATA_DIR, d)))
frames = {}
for s in sessions:
    folder = os.path.join(DATA_DIR, s)
    calib = load_calib(folder)
    takes = [load_csv(p, calib) for p in sorted(glob.glob(os.path.join(folder, "*.csv")))]
    remove_gyro_bias(takes, calib)     # same as dataset.py, so the table shows what the model sees
    frames[s] = np.concatenate(takes)

train = [s for s in sessions if s not in VAL_SESSIONS + TEST_SESSIONS]
pooled = np.concatenate([frames[s] for s in train])
t_mean, t_std = pooled.mean(0), pooled.std(0) + 1e-6

def tag(s):
    return "test" if s in TEST_SESSIONS else "val" if s in VAL_SESSIONS else "train"

w = 15
print("channel  " + "".join(f"{s[:w]:>{w}s}" for s in sessions))
print("         " + "".join(f"{'(' + tag(s) + ')':>{w}s}" for s in sessions))
flagged = {s: 0 for s in sessions}
for c, name in enumerate(CH_NAMES):
    cells = []
    for s in sessions:
        m = frames[s][:, c].mean()
        far = abs(m - t_mean[c]) > t_std[c]
        flagged[s] += far
        cells.append(f"{m:{w - 1}.2f}{'*' if far else ' '}")
    print(f"{name:8s} " + "".join(cells))

print("\nchannels far from the training mean (*):")
for s in sessions:
    print(f"  {s:20s} ({tag(s)}): {flagged[s]}/{len(CH_NAMES)}")