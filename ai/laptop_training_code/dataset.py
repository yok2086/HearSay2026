"""
Glove data loader for the TCN project.

Data layout (copy the logger's session folders into data/glove/):
  data/glove/<session>/
  ├── calib.json     {"left": {"straight": [5], "bent": [5]}, "right": {...}}
  ├── hello-01.csv   L_millis,L_ax,...,L_f5,R_millis,R_ax,...,R_f5
  └── ...

- Split by person/session: test = nathan (unseen person), val = darius_22_oct,
  train = every other session folder.
- Each CSV -> (T, 22): per hand ax, ay, az, gx, gy, gz, then flex thumb..pinky,
  left hand first. Flex is normalised per session with calib.json:
  (raw - straight) / (bent - straight), so 0 = straight, 1 = fist (not clipped).
  millis columns are dropped.
- Gyro zero-rate offset removed per session: the MPU-6050's offset changes at
  every power-up, so each session's offset is estimated as the median gyro over
  the first STILL_FRAMES frames of its takes (the pause before each sign) and
  subtracted. Live, measure it during the straight-hand calibration hold.
- Padded/cropped to WINDOW frames -> (22, WINDOW).
- Per-channel z-score computed on TRAINING frames only. train.py saves it to
  norm_stats.npz; every other script loads that file (the board needs the same
  mean/std, and the same flex calibration, for live data).
- train.py --final puts every session into training, for the deployed model.

Run `python dataset.py` for a self-check (needs only numpy).
"""
import os
import json
import glob
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.join(HERE, "..", "training_data")
NORM_PATH = os.path.join(HERE, "norm_stats.npz")

WINDOW = 256                            # frames per window (~2.56 s at 100 Hz); longest take is 231
HANDS = ("left", "right")
PREFIX = {"left": "L", "right": "R"}
IMU = ["ax", "ay", "az", "gx", "gy", "gz"]
FLEX = ["f1", "f2", "f3", "f4", "f5"]   # thumb, index, middle, ring, pinky
N_CH = len(HANDS) * (len(IMU) + len(FLEX))   # 22
PER_HAND = len(IMU) + len(FLEX)
GYRO_CH = [h * PER_HAND + i for h in range(len(HANDS)) for i in (3, 4, 5)]   # gx, gy, gz of each hand
FLEX_CH = [h * PER_HAND + len(IMU) + i for h in range(len(HANDS)) for i in range(len(FLEX))]
ACC_CH = [h * PER_HAND + i for h in range(len(HANDS)) for i in (0, 1, 2)]
STILL_FRAMES = 10       # frames at the start of each take treated as still (0.1 s)

SIGNS = [
    "hello", "see_you_later", "me", "yes", "no", "help", "please", "thank_you",
    "want", "what", "again", "eat_food", "more", "go_to", "toilet", "fine",
    "like", "learn", "sign", "finish",
]
SIGN_TO_IDX = {s: i for i, s in enumerate(SIGNS)}

VAL_SESSIONS = ["darius_31_oct"]
TEST_SESSIONS = ["ryan_3_nov"]
MIN_FLEX_RANGE = 50     # ADC counts; a smaller calibration range would blow up the division


def sessions_for(split):
    """Session folders in a split. Train = everything not in val or test."""
    found = sorted(d for d in os.listdir(DATA_DIR) if os.path.isdir(os.path.join(DATA_DIR, d)))
    for s in VAL_SESSIONS + TEST_SESSIONS:
        if s not in found:
            raise FileNotFoundError(f"session folder {s} not found in {DATA_DIR}")
    if split == "val":
        return list(VAL_SESSIONS)
    if split == "test":
        return list(TEST_SESSIONS)
    return [s for s in found if s not in VAL_SESSIONS + TEST_SESSIONS]


def load_calib(session_dir):
    """Per-hand (straight, range) arrays from calib.json, plus "gyro_offset"
    (gx, gy, gz per hand, left first) if the logger saved one."""
    path = os.path.join(session_dir, "calib.json")
    with open(path) as f:
        calib = json.load(f)
    out = {}
    for hand in HANDS:
        if hand not in calib:
            raise ValueError(f"{path}: no '{hand}' entry (was this session recorded with both gloves?)")
        if "gyro_offset" in calib[hand]:
            out.setdefault("gyro_offset", []).extend(calib[hand]["gyro_offset"])
        straight = np.array(calib[hand]["straight"], dtype=np.float32)
        bent = np.array(calib[hand]["bent"], dtype=np.float32)
        rng = bent - straight
        weak = np.abs(rng) < MIN_FLEX_RANGE
        if weak.any():
            print(f"  WARNING {path}: small flex range for {hand} finger(s) {np.where(weak)[0].tolist()}, "
                  f"clamped to {MIN_FLEX_RANGE}")
            rng = np.where(weak, np.where(rng < 0, -MIN_FLEX_RANGE, MIN_FLEX_RANGE), rng)
        out[hand] = (straight, rng)
    return out


def load_csv(path, calib):
    """One recording -> (T, 22) float32 with calibrated flex."""
    with open(path) as f:
        header = f.readline().strip().split(",")
    x = np.loadtxt(path, delimiter=",", skiprows=1, dtype=np.float32, ndmin=2)
    if x.shape[1] != len(header):
        raise ValueError(f"{path}: {x.shape[1]} columns but header has {len(header)}")
    col = {name: i for i, name in enumerate(header)}
    parts = []
    for hand in HANDS:
        p = PREFIX[hand]
        try:
            imu = x[:, [col[f"{p}_{c}"] for c in IMU]]
            flex = x[:, [col[f"{p}_{c}"] for c in FLEX]]
        except KeyError as e:
            raise ValueError(f"{path}: missing column {e} (one-glove recording?)") from None
        straight, rng = calib[hand]
        parts += [imu, (flex - straight) / rng]
    return np.concatenate(parts, axis=1)


def remove_gyro_bias(takes, calib=None):
    """Subtract one session's gyro offset from all its takes (in place).
    Uses the offset measured during calibration if calib.json has one (newer
    sessions); otherwise estimates it as the median over takes of the median
    gyro in each take's first STILL_FRAMES (the pause before each sign)."""
    if calib is not None and len(calib.get("gyro_offset", [])) == len(GYRO_CH):
        bias = np.array(calib["gyro_offset"], dtype=np.float32)
    else:
        starts = np.stack([np.median(x[:STILL_FRAMES, GYRO_CH], axis=0) for x in takes])
        bias = np.median(starts, axis=0)
    for x in takes:
        x[:, GYRO_CH] -= bias
    return bias


def fit_window(x, length=WINDOW):
    """Centre-pad with repeated first/last frames, or centre-crop if too long."""
    T = x.shape[0]
    if T >= length:
        start = (T - length) // 2
        return x[start:start + length]
    before = (length - T) // 2
    after = length - T - before
    return np.pad(x, ((before, after), (0, 0)), mode="edge")


def load_split(split):
    """Returns X (N, 22, 96) un-normalised, y (N,), list of raw (T, 22) arrays, list of paths."""
    xs, ys, raw, paths = [], [], [], []
    for session in sessions_for(split):
        folder = os.path.join(DATA_DIR, session)
        calib = load_calib(folder)
        takes, labels, files = [], [], []
        for path in sorted(glob.glob(os.path.join(folder, "*.csv"))):
            sign = os.path.basename(path)[:-4].rsplit("-", 1)[0]
            if sign not in SIGN_TO_IDX:
                print(f"  skipping {session}/{os.path.basename(path)} (unknown sign)")
                continue
            takes.append(load_csv(path, calib))
            labels.append(SIGN_TO_IDX[sign])
            files.append(path)
        if not takes:
            continue
        remove_gyro_bias(takes, calib)
        for x, label, path in zip(takes, labels, files):
            raw.append(x)
            xs.append(fit_window(x).T)          # (22, WINDOW)
            ys.append(label)
            paths.append(path)
    if not xs:
        raise ValueError(f"no recordings found for split '{split}'")
    return np.stack(xs).astype(np.float32), np.array(ys, dtype=np.int64), raw, paths


def compute_norm(raw_train):
    """Mean/std over real (unpadded) training frames only."""
    frames = np.concatenate(raw_train, axis=0)   # (total_frames, 22)
    mean = frames.mean(axis=0).astype(np.float32)
    std = (frames.std(axis=0) + 1e-6).astype(np.float32)
    return mean, std


def normalise(X, mean, std):
    return (X - mean[None, :, None]) / std[None, :, None]


def load_all(final=False, fit_norm=False):
    """Returns {split: (X_normalised, y)}.

    fit_norm=True  (train.py): compute the z-score mean/std from the training
                   data and save them to norm_stats.npz.
    fit_norm=False (evaluate.py, quantize.py, ...): load norm_stats.npz, so every
                   script normalises exactly the way the saved model was trained.
    final=True     (train.py --final): the training split is every session
                   (train + val + test), for the model that gets deployed.
    """
    splits = {s: load_split(s) for s in ("train", "val", "test")}
    parts = [splits[s] for s in (("train", "val", "test") if final else ("train",))]
    Xtr = np.concatenate([p[0] for p in parts])
    ytr = np.concatenate([p[1] for p in parts])
    raw_tr = [r for p in parts for r in p[2]]

    if fit_norm:
        mean, std = compute_norm(raw_tr)
        np.savez(NORM_PATH, mean=mean, std=std, final=final)
    else:
        if not os.path.exists(NORM_PATH):
            raise FileNotFoundError(f"{NORM_PATH} not found - run train.py first")
        stats = np.load(NORM_PATH)
        mean, std = stats["mean"], stats["std"]
        if "final" in stats and bool(stats["final"]):
            print("  note: norm_stats.npz is from a --final run, so val/test were used in training "
                  "and their scores are not a real test")

    data = {"train": (normalise(Xtr, mean, std), ytr)}
    for split in ("val", "test"):
        X, y = splits[split][:2]
        data[split] = (normalise(X, mean, std), y)
    return data


def get_torch_datasets(final=False, fit_norm=False):
    import torch
    from torch.utils.data import TensorDataset
    data = load_all(final=final, fit_norm=fit_norm)
    return {s: TensorDataset(torch.from_numpy(X), torch.from_numpy(y))
            for s, (X, y) in data.items()}


if __name__ == "__main__":
    ok = True
    lengths = []
    for split in ("train", "val", "test"):
        X, y, raw, _ = load_split(split)
        lengths += [r.shape[0] for r in raw]
        counts = np.bincount(y, minlength=len(SIGNS))
        print(f"{split:5s}  sessions={sessions_for(split)}")
        print(f"       X={X.shape}  per-sign min/max = {counts.min()}/{counts.max()}")
        if counts.min() == 0:
            ok = False
            print(f"       !! no recordings for: {[SIGNS[i] for i, c in enumerate(counts) if c == 0]}")

    L = np.array(lengths)
    over = int((L > WINDOW).sum())
    print(f"\ntake length (frames): min {L.min()}  median {int(np.median(L))}  "
          f"90th {int(np.percentile(L, 90))}  95th {int(np.percentile(L, 95))}  max {L.max()}")
    print(f"takes longer than WINDOW={WINDOW}: {over}/{len(L)} ({100 * over / len(L):.0f}%) - these get centre-cropped")

    # check normalisation without overwriting norm_stats.npz (only train.py saves it)
    Xtr, _, raw_tr, _ = load_split("train")
    mean, std = compute_norm(raw_tr)
    Xn = normalise(Xtr, mean, std)
    print(f"train normalised: mean={Xn.mean():.3f}  std={Xn.std():.3f}  (expect ~0 / ~1)")

    try:
        import model
        if (model.N_CH, model.N_CLASSES) != (N_CH, len(SIGNS)):
            ok = False
            print(f"!! model.py has N_CH={model.N_CH}, N_CLASSES={model.N_CLASSES}; "
                  f"expected {N_CH}, {len(SIGNS)}")
    except ImportError:
        print("(torch not available here - skipped model.py check)")

    print("ALL CHECKS PASSED" if ok else "CHECK FAILED - see above")
