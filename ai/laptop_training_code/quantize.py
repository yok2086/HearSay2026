"""
Quantize the float TCN to INT8 with vai_q_pytorch (Vitis AI 2.5).

Two passes:
  python quantize.py --mode calib   # measure ranges on training data
  python quantize.py --mode test    # INT8 test accuracy + export .xmodel

Output: quantize_result/TCN_int.xmodel  (input for the compiler)
"""
import argparse
import torch
from torch.utils.data import DataLoader
from pytorch_nndct.apis import torch_quantizer

from dataset import get_torch_datasets
from model import TCN, N_CH, WINDOW

CKPT = "tcn_float.pth"
OUT_DIR = "quantize_result"


def to_4d(x):
    # (N, 22, 96) -> (N, 22, 1, 96), the layout the DPU model is built for
    return x.unsqueeze(2)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["calib", "test"], required=True)
    args = parser.parse_args()

    device = torch.device("cpu")
    model = TCN()
    model.load_state_dict(torch.load(CKPT, map_location="cpu"))
    model.eval()

    dummy = torch.randn(1, N_CH, 1, WINDOW)
    quantizer = torch_quantizer(args.mode, model, (dummy,), device=device, output_dir=OUT_DIR)
    qmodel = quantizer.quant_model
    qmodel.eval()

    ds = get_torch_datasets()

    if args.mode == "calib":
        loader = DataLoader(ds["train"], batch_size=32)
        with torch.no_grad():
            for x, _ in loader:
                qmodel(to_4d(x))
        quantizer.export_quant_config()
        print(f"calibration done -> config saved in {OUT_DIR}/")

    else:
        # batch size 1 is required for xmodel export
        loader = DataLoader(ds["test"], batch_size=1)
        correct, total = 0, 0
        with torch.no_grad():
            for x, y in loader:
                out = qmodel(to_4d(x))
                correct += (out.argmax(1) == y).sum().item()
                total += y.size(0)
        print(f"INT8 (quantized) test accuracy: {correct}/{total} = {correct / total:.3f}")
        quantizer.export_xmodel(output_dir=OUT_DIR, deploy_check=False)
        print(f"xmodel exported -> {OUT_DIR}/TCN_int.xmodel")


if __name__ == "__main__":
    main()
