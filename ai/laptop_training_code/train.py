"""
Train the TCN on the glove data.

- Trains on the train sessions, checks val every epoch
- Saves the checkpoint with the best val accuracy to tcn_float.pth
- Test split is NOT touched here (evaluated once, separately)
- Every training take gets a fresh random augmentation each time it is used
  (see augment()). Nothing is copied or saved; val and test are never augmented.

Run: python train.py               # evaluation run: train sessions only, best-val epoch saved
     python train.py --final       # deployment run: ALL sessions, fixed EPOCHS, last epoch saved
     python train.py --check-aug   # sanity-check the augmentation, no training

Both runs save tcn_float.pth and norm_stats.npz (the z-score stats of the data
trained on). Quote test accuracy from an evaluation run; a --final model has
no unseen data left, so its val/test scores are not a real test.
"""
import sys
import math
import time
import random
import numpy as np
import torch
import torch.nn as nn
from torch.utils.data import DataLoader

from dataset import get_torch_datasets, sessions_for, NORM_PATH, HANDS, PER_HAND, FLEX_CH
from model import TCN

# ---- settings ----
SEED = 0
EPOCHS = 500
BATCH = 32
LR = 1e-3
WEIGHT_DECAY = 1e-3
OUT_PATH = "tcn_float.pth"

# ---- augmentation (amounts in real units: m/s^2, deg/s, calibrated flex) ----
AUGMENT = True
MAX_ROT_DEG = 15.0          # per-hand IMU rotation: posture, glove fit, mounting angle
MAX_SHIFT = 25              # frames: sign position in the window
SPEED = (0.85, 1.15)        # signing speed
FLEX_SCALE = (0.8, 1.2)     # flex calibration gain error
FLEX_OFFSET = 0.4           # flex calibration offset error (+-; 1 = straight -> fist)
ACC_SCALE = (0.95, 1.05)    # accelerometer gain differences
GYRO_OFFSET = 2.0           # small leftover gyro offset (+- deg/s)
NOISE = 0.05                # Gaussian noise, in z-scored units

ACC = [[h * PER_HAND + i for i in (0, 1, 2)] for h in range(len(HANDS))]    # per hand
GYRO = [[h * PER_HAND + i for i in (3, 4, 5)] for h in range(len(HANDS))]


def set_seed(seed):
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)


def random_rotations(n, max_deg):
    """n random 3x3 rotation matrices: random axis, angle uniform in [0, max_deg]."""
    axis = torch.randn(n, 3)
    axis = axis / axis.norm(dim=1, keepdim=True)
    angle = torch.rand(n) * math.radians(max_deg)
    K = torch.zeros(n, 3, 3)                                   # cross-product matrix of axis
    K[:, 0, 1], K[:, 0, 2] = -axis[:, 2], axis[:, 1]
    K[:, 1, 0], K[:, 1, 2] = axis[:, 2], -axis[:, 0]
    K[:, 2, 0], K[:, 2, 1] = -axis[:, 1], axis[:, 0]
    s, c = angle.sin()[:, None, None], angle.cos()[:, None, None]
    return torch.eye(3).expand(n, 3, 3) + s * K + (1 - c) * (K @ K)   # Rodrigues


def make_augment(mean, std):
    """Returns augment(x) for a batch x of z-scored windows, shape (B, C, W).
    Works in real units (undo z-score, augment, redo z-score)."""
    m = torch.from_numpy(mean)[None, :, None]
    sd = torch.from_numpy(std)[None, :, None]

    def augment(x):
        B, C, W = x.shape
        x = x * sd + m                                         # back to real units

        # time: stretch around the centre + shift; edge frames repeated, like the padding
        t = torch.arange(W, dtype=torch.float32)
        c = (W - 1) / 2
        speed = torch.empty(B, 1).uniform_(*SPEED)
        shift = torch.randint(-MAX_SHIFT, MAX_SHIFT + 1, (B, 1)).float()
        src = (c + (t - c) * speed - shift).round().clamp(0, W - 1).long()
        x = torch.gather(x, 2, src.unsqueeze(1).expand(B, C, W))

        # IMU: one random rotation per hand, applied to accel and gyro together
        for acc, gyro in zip(ACC, GYRO):
            R = random_rotations(B, MAX_ROT_DEG)               # (B, 3, 3)
            x[:, acc] = R @ x[:, acc]
            x[:, gyro] = R @ x[:, gyro]
            x[:, acc] *= torch.empty(B, 3, 1).uniform_(*ACC_SCALE)
            x[:, gyro] += torch.empty(B, 3, 1).uniform_(-GYRO_OFFSET, GYRO_OFFSET)

        # flex: calibration gain and offset errors, per finger
        n = len(FLEX_CH)
        x[:, FLEX_CH] = (x[:, FLEX_CH] * torch.empty(B, n, 1).uniform_(*FLEX_SCALE)
                         + torch.empty(B, n, 1).uniform_(-FLEX_OFFSET, FLEX_OFFSET))

        x = (x - m) / sd                                       # back to z-scored
        return x + NOISE * torch.randn_like(x)

    return augment


def check_augment(augment, ds):
    """Quick sanity check: shapes, finite values, rotation keeps |accel| unchanged."""
    global MAX_SHIFT, SPEED, ACC_SCALE, FLEX_OFFSET, NOISE
    x = ds["train"].tensors[0][:64]
    y = augment(x.clone())
    print(f"shape {tuple(x.shape)} -> {tuple(y.shape)}, all finite: {torch.isfinite(y).all().item()}")
    print(f"z-scored std before {x.std():.3f}, after {y.std():.3f}")

    saved = MAX_SHIFT, SPEED, ACC_SCALE, NOISE
    MAX_SHIFT, SPEED, ACC_SCALE, NOISE = 0, (1.0, 1.0), (1.0, 1.0), 0.0
    stats = np.load(NORM_PATH)
    m = torch.from_numpy(stats["mean"])[None, :, None]
    sd = torch.from_numpy(stats["std"])[None, :, None]
    y = augment(x.clone()) * sd + m
    xr = x * sd + m
    for h, acc in enumerate(ACC):
        diff = (y[:, acc].norm(dim=1) - xr[:, acc].norm(dim=1)).abs().max()
        print(f"{HANDS[h]:5s}: max change in |accel| after rotation only = {diff:.5f} m/s^2 (expect ~0)")
    MAX_SHIFT, SPEED, ACC_SCALE, NOISE = saved


def evaluate(model, loader, loss_fn):
    model.eval()
    correct, total, loss_sum = 0, 0, 0.0
    with torch.no_grad():
        for x, y in loader:
            out = model(x)
            loss_sum += loss_fn(out, y).item() * y.size(0)
            correct += (out.argmax(1) == y).sum().item()
            total += y.size(0)
    return correct / total, loss_sum / total


def main():
    final = "--final" in sys.argv
    set_seed(SEED)
    ds = get_torch_datasets(final=final, fit_norm=True)    # also writes norm_stats.npz
    stats = np.load(NORM_PATH)
    augment = make_augment(stats["mean"], stats["std"])
    if "--check-aug" in sys.argv:
        check_augment(augment, ds)
        return
    if not AUGMENT:
        augment = lambda x: x

    if final:
        sessions = sessions_for("train") + sessions_for("val") + sessions_for("test")
        print(f"FINAL run: training on all {len(sessions)} sessions {sessions}, "
              f"{EPOCHS} epochs, last epoch saved, no validation")
    train_loader = DataLoader(ds["train"], batch_size=BATCH, shuffle=True)
    val_loader = DataLoader(ds["val"], batch_size=BATCH)

    model = TCN()
    loss_fn = nn.CrossEntropyLoss()
    opt = torch.optim.Adam(model.parameters(), lr=LR, weight_decay=WEIGHT_DECAY)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=EPOCHS)

    best_acc, best_epoch = 0.0, 0
    start = time.time()

    for epoch in range(1, EPOCHS + 1):
        model.train()
        correct, total, loss_sum = 0, 0, 0.0
        for x, y in train_loader:
            x = augment(x)
            opt.zero_grad()
            out = model(x)
            loss = loss_fn(out, y)
            loss.backward()
            opt.step()
            loss_sum += loss.item() * y.size(0)
            correct += (out.argmax(1) == y).sum().item()
            total += y.size(0)
        sched.step()

        train_acc, train_loss = correct / total, loss_sum / total

        if final:
            if epoch == 1 or epoch % 10 == 0:
                print(f"epoch {epoch:3d}  train loss {train_loss:.3f} acc {train_acc:.3f}")
            continue

        val_acc, val_loss = evaluate(model, val_loader, loss_fn)
        if val_acc > best_acc:
            best_acc, best_epoch = val_acc, epoch
            torch.save(model.state_dict(), OUT_PATH)

        if epoch == 1 or epoch % 10 == 0:
            print(f"epoch {epoch:3d}  train loss {train_loss:.3f} acc {train_acc:.3f}  "
                  f"val loss {val_loss:.3f} acc {val_acc:.3f}")

    print(f"\ndone in {time.time() - start:.0f}s")
    if final:
        torch.save(model.state_dict(), OUT_PATH)
        print(f"final model (epoch {EPOCHS}) -> saved {OUT_PATH}, norm stats -> {NORM_PATH}")
    else:
        print(f"best val acc {best_acc:.3f} at epoch {best_epoch}  -> saved {OUT_PATH}")


if __name__ == "__main__":
    main()
