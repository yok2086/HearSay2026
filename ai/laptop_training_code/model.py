"""
TCN for 20-sign classification from (22 channels x 96 frames) windows.

Uses Conv2d with (1, k) kernels on input shaped (N, 22, 1, 96) instead of Conv1d.
Mathematically identical, but maps cleanly onto the DPU (a 2D conv engine).

No dilation: the DPUCZDX8G compiler rejects dilation larger than the input
height (which is 1 here). The receptive field is grown with stride-2
downsampling instead: 96 -> 48 -> 24 -> 12 -> 6 frames.

DPU-safe ops only: Conv2d, BatchNorm2d, ReLU, residual add, average pool, Linear.
Dropout is used in training only and disappears at inference.

Run `python model.py` for a self-check.
"""
import torch
import torch.nn as nn

N_CH = 22
N_CLASSES = 20
WINDOW = 256


class ResBlock(nn.Module):
    """Conv -> BN -> ReLU -> Conv -> BN, add skip, ReLU.
    With stride 2, the first conv halves the time length and the skip path
    uses a strided 1x1 conv so the shapes match for the add."""

    def __init__(self, ch, stride=1, k=3):
        super().__init__()
        pad = (k - 1) // 2
        self.conv1 = nn.Conv2d(ch, ch, (1, k), stride=(1, stride), padding=(0, pad), bias=False)
        self.bn1 = nn.BatchNorm2d(ch)
        self.relu1 = nn.ReLU()
        self.conv2 = nn.Conv2d(ch, ch, (1, k), padding=(0, pad), bias=False)
        self.bn2 = nn.BatchNorm2d(ch)
        self.relu2 = nn.ReLU()
        if stride == 1:
            self.skip = nn.Identity()
        else:
            self.skip = nn.Sequential(
                nn.Conv2d(ch, ch, (1, 1), stride=(1, stride), bias=False),
                nn.BatchNorm2d(ch),
            )

    def forward(self, x):
        y = self.relu1(self.bn1(self.conv1(x)))
        y = self.bn2(self.conv2(y))
        return self.relu2(y + self.skip(x))


class TCN(nn.Module):
    def __init__(self, in_ch=N_CH, ch=16, n_classes=N_CLASSES,
                 strides=(1, 2, 2, 2, 2, 2), dropout=0.4):
        super().__init__()
        # 1x1 stem: lifts 22 input channels to `ch` so every block can use a skip-add
        self.stem = nn.Sequential(
            nn.Conv2d(in_ch, ch, (1, 1), bias=False),
            nn.BatchNorm2d(ch),
            nn.ReLU(),
        )
        self.blocks = nn.Sequential(*[ResBlock(ch, s) for s in strides])
        self.pool = nn.AdaptiveAvgPool2d((1, 1))   # global average over time
        self.drop = nn.Dropout(dropout)
        self.fc = nn.Linear(ch, n_classes)

    def forward(self, x):
        if x.dim() == 3:              # accept (N, 22, 96) from the data loader
            x = x.unsqueeze(2)        # -> (N, 22, 1, 96)
        x = self.stem(x)
        x = self.blocks(x)
        x = self.pool(x)
        x = torch.flatten(x, 1)
        x = self.drop(x)
        return self.fc(x)


def receptive_field(strides=(1, 2, 2, 2, 2, 2), k=3):
    """Frames of the original input seen by one output position before pooling."""
    rf, jump = 1, 1
    for s in strides:
        rf += (k - 1) * jump      # conv1 (applied before its stride takes effect)
        jump *= s
        rf += (k - 1) * jump      # conv2
    return rf


if __name__ == "__main__":
    model = TCN()
    n_params = sum(p.numel() for p in model.parameters())
    print(f"parameters:      {n_params:,}")
    print(f"receptive field: {receptive_field()} frames (window = {WINDOW})")

    model.eval()
    with torch.no_grad():
        x4 = torch.randn(4, N_CH, 1, WINDOW)
        t = model.stem(x4)
        lengths = [t.shape[-1]]
        for b in model.blocks:
            t = b(t)
            lengths.append(t.shape[-1])
        out3 = model(torch.randn(4, N_CH, WINDOW))
        out4 = model(x4)
    print(f"time length:     {' -> '.join(map(str, lengths))}")
    print(f"output (3D in):  {tuple(out3.shape)}")
    print(f"output (4D in):  {tuple(out4.shape)}")

    ok = out3.shape == (4, N_CLASSES) and out4.shape == (4, N_CLASSES)
    print("MODEL CHECK PASSED" if ok else "MODEL CHECK FAILED")
