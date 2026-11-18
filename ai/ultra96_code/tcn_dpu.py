"""
Run the compiled TCN on the Ultra96 DPU.

- Accuracy on the test windows in test_data.npz
- DPU latency / throughput (single-window inference)

Must run as root with the XRT and PYNQ environments sourced:
  sudo su
  . /etc/profile.d/xrt_setup.sh
  . /etc/profile.d/pynq_venv.sh
  cd /home/xilinx/jupyter_notebooks/tcn
  python3 tcn_dpu.py
"""
import time
import numpy as np
from pynq_dpu import DpuOverlay

from tcn_infer import SIGNS

N_TIMING_RUNS = 1000

# ---- load DPU and model ----
overlay = DpuOverlay("dpu.bit")
overlay.load_model("tcn.xmodel")
dpu = overlay.runner

in_t = dpu.get_input_tensors()[0]
out_t = dpu.get_output_tensors()[0]
shape_in = tuple(in_t.dims)
shape_out = tuple(out_t.dims)
print(f"DPU input tensor:  {in_t.name}  {shape_in}")
print(f"DPU output tensor: {out_t.name}  {shape_out}")

# DPU tensors are NHWC: (1, 1, frames, channels)
frames, channels = shape_in[2], shape_in[3]
assert shape_in[1] == 1, f"unexpected input shape {shape_in}"
assert shape_out[-1] == len(SIGNS), f"model has {shape_out[-1]} outputs, SIGNS has {len(SIGNS)}"

input_data = [np.empty(shape_in, dtype=np.float32, order="C")]
output_data = [np.empty(shape_out, dtype=np.float32, order="C")]


def run_one(window):
    """window: (channels, frames) normalised -> class index"""
    input_data[0][0] = window.T.reshape(shape_in[1:])   # (22, 256) -> (1, 256, 22)
    job = dpu.execute_async(input_data, output_data)
    dpu.wait(job)
    return int(np.argmax(output_data[0][0]))


# ---- accuracy ----
data = np.load("test_data.npz")
X, y = data["X"], data["y"]
assert X.shape[1:] == (channels, frames), f"test_data is {X.shape[1:]}, model expects {(channels, frames)}"
print(f"\ntest set: {X.shape[0]} windows of {channels} channels x {frames} frames")

preds = np.array([run_one(X[i]) for i in range(X.shape[0])])
correct = int((preds == y).sum())
print(f"DPU test accuracy: {correct}/{len(y)} = {correct / len(y):.3f}")
for i in np.nonzero(preds != y)[0]:
    print(f"  sample {i}: true {SIGNS[y[i]]} -> predicted {SIGNS[preds[i]]}")

# ---- latency ----
input_data[0][0] = X[0].T.reshape(shape_in[1:])
for _ in range(20):                                   # warm-up
    dpu.wait(dpu.execute_async(input_data, output_data))

times = []
for _ in range(N_TIMING_RUNS):
    t0 = time.perf_counter()
    dpu.wait(dpu.execute_async(input_data, output_data))
    times.append(time.perf_counter() - t0)
times_ms = np.array(times) * 1000

print(f"\nDPU latency over {N_TIMING_RUNS} runs (single window, incl. data transfer):")
print(f"  mean   {times_ms.mean():.3f} ms")
print(f"  median {np.median(times_ms):.3f} ms")
print(f"  p99    {np.percentile(times_ms, 99):.3f} ms")
print(f"  throughput ~{1000 / times_ms.mean():.0f} windows/s")
