"""
Evaluate the trained float model on the test split (days 8-9).

Run: python evaluate.py
"""
import torch
from torch.utils.data import DataLoader

from dataset import get_torch_datasets, SIGNS
from model import TCN

CKPT = "tcn_float.pth"


def main():
    ds = get_torch_datasets()
    loader = DataLoader(ds["test"], batch_size=64)

    model = TCN()
    model.load_state_dict(torch.load(CKPT, map_location="cpu"))
    model.eval()

    preds, labels = [], []
    with torch.no_grad():
        for x, y in loader:
            preds.append(model(x).argmax(1))
            labels.append(y)
    preds = torch.cat(preds)
    labels = torch.cat(labels)

    correct = (preds == labels).sum().item()
    total = labels.numel()
    print(f"float model test accuracy: {correct}/{total} = {correct / total:.3f}")

    wrong = (preds != labels).nonzero().flatten().tolist()
    if wrong:
        print("\nmisclassified:")
        for i in wrong:
            print(f"  true {SIGNS[labels[i]]:14s} -> predicted {SIGNS[preds[i]]}")

    
    # ---- confusion matrix ----
    # rows = true sign, columns = predicted sign, number = how many test windows
    n = len(SIGNS)
    cm = torch.zeros(n, n, dtype=torch.int64)
    for t, p in zip(labels, preds):
        cm[t, p] += 1

    print("\nconfusion matrix (rows = true sign, columns = predicted class index):")
    print(" " * 15 + "".join(f"{j:3d}" for j in range(n)))
    for i in range(n):
        print(f"{i:2d} {SIGNS[i]:12s}" + "".join(f"{cm[i, j].item():3d}" for j in range(n)))


if __name__ == "__main__":
    main()
