"""
Reusable DPU inference module for the TCN sign classifier.

Usage from any other Python file on the board:
    import sys
    sys.path.insert(0, "/home/xilinx/jupyter_notebooks/tcn")
    from tcn_infer import TcnDpu, SIGNS

    clf = TcnDpu()                          # loads overlay + model once
    label, idx, conf = clf.predict(window)  # window: (22, 96) normalised float array

Must run as root with XRT and PYNQ environments sourced.
"""
import os
import numpy as np
from pynq_dpu import DpuOverlay

HERE = os.path.dirname(os.path.abspath(__file__))

SIGNS = [
    "hello", "later", "I", "yes", "no", "please", "thank", "what", "eat", "more",
    "go", "love", "happy", "know", "ready", "deaf", "you", "where", "sorry", "wait_notyet_",
]


class TcnDpu:
    def __init__(self, xmodel=os.path.join(HERE, "tcn.xmodel")):
        self.overlay = DpuOverlay("dpu.bit")
        self.overlay.load_model(xmodel)
        self.dpu = self.overlay.runner
        self.shape_in = tuple(self.dpu.get_input_tensors()[0].dims)    # (1, 1, 96, 22)
        self.shape_out = tuple(self.dpu.get_output_tensors()[0].dims)  # (1, 20)
        self.inp = [np.empty(self.shape_in, dtype=np.float32, order="C")]
        self.out = [np.empty(self.shape_out, dtype=np.float32, order="C")]

    def scores(self, window):
        """window: (22, 96) normalised -> (20,) class probabilities"""
        self.inp[0][0] = window.T.reshape(self.shape_in[1:])   # (22, 96) -> (1, 96, 22)
        self.dpu.wait(self.dpu.execute_async(self.inp, self.out))
        logits = self.out[0][0].astype(np.float64)
        p = np.exp(logits - logits.max())
        return p / p.sum()

    def predict(self, window):
        """window: (22, 96) normalised -> (sign name, class index, confidence 0-1)"""
        p = self.scores(window)
        i = int(p.argmax())
        return SIGNS[i], i, float(p[i])


def load_test_data():
    """Returns (X, y): X (120, 22, 96) normalised windows, y (120,) class indices."""
    d = np.load(os.path.join(HERE, "test_data.npz"))
    return d["X"], d["y"]
