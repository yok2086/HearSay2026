"""
Float-model accuracy on train / val / test, plus a test-set confusion matrix.

Run (in the container, env vitis-ai-pytorch, from tcn_project):
    python confusion_matrix.py
Prints train / val / test accuracy and saves confusion_matrix.png
"""
import numpy as np
import torch

from dataset import load_all, SIGNS
from model import TCN

CKPT = "tcn_float.pth"

model = TCN()
model.load_state_dict(torch.load(CKPT, map_location="cpu"))
model.eval()

data = load_all()
for split in ("train", "val", "test"):
    X, y = data[split]
    with torch.no_grad():
        pred = model(torch.from_numpy(X)).argmax(1).numpy()
    print(f"{split:5s} accuracy: {(pred == y).sum()}/{len(y)} = {(pred == y).mean():.3f}")
    if split == "test":
        y_test, p_test = y, pred

n = len(SIGNS)
cm = np.zeros((n, n), dtype=int)
for t, p in zip(y_test, p_test):
    cm[t, p] += 1

wrong = [(SIGNS[t], SIGNS[p]) for t, p in zip(y_test, p_test) if t != p]
print("\ntest misclassifications:", wrong if wrong else "none")

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(9, 8))
    ax.imshow(cm, cmap="Blues")
    ax.set_xticks(range(n))
    ax.set_yticks(range(n))
    ax.set_xticklabels(SIGNS, rotation=90)
    ax.set_yticklabels(SIGNS)
    ax.set_xlabel("Predicted sign")
    ax.set_ylabel("True sign")
    ax.set_title(f"Test confusion matrix (float model): {(p_test == y_test).sum()}/{len(y_test)} correct")
    for i in range(n):
        for j in range(n):
            if cm[i, j]:
                ax.text(j, i, cm[i, j], ha="center", va="center",
                        color="white" if cm[i, j] > cm.max() / 2 else "black", fontsize=8)
    fig.tight_layout()
    fig.savefig("confusion_matrix.png", dpi=150)
    print("saved confusion_matrix.png")
except ImportError:
    print("matplotlib not available - printing matrix instead")
    print(cm)
