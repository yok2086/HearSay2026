"""
Run the compiled TCN on the Ultra96 DPU.

- Accuracy on the 120 test windows (days 8-9)
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

# DPU tensors are NHWC: expect (1, 1, 96, 22) = batch, height, time, channels
assert shape_in[1:] == (1, 96, 22), f"unexpected input shape {shape_in}"

input_data = [np.empty(shape_in, dtype=np.float32, order="C")]
output_data = [np.empty(shape_out, dtype=np.float32, order="C")]


def run_one(window):
    """window: (22, 96) normalised -> class index"""
    input_data[0][0] = window.T.reshape(shape_in[1:])   # (22, 96) -> (1, 96, 22)
    job = dpu.execute_async(input_data, output_data)
    dpu.wait(job)
    return int(np.argmax(output_data[0][0]))


# ---- accuracy ----
data = np.load("test_data.npz")
X, y = data["X"], data["y"]
print(f"\ntest set: {X.shape[0]} windows")

preds = np.array([run_one(X[i]) for i in range(X.shape[0])])
correct = int((preds == y).sum())
print(f"DPU test accuracy: {correct}/{len(y)} = {correct / len(y):.3f}")
wrong = np.nonzero(preds != y)[0]
for i in wrong:
    print(f"  sample {i}: true class {y[i]} -> predicted {preds[i]}")

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
