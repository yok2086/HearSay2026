"""
Power management test for the TCN on the Ultra96-V2.

Measures the board's power rails and DPU latency in several states:
  1. DPU idle            (overlay + model loaded, nothing running)
  2. DPU running         (continuous inference)
  3. CPU at min freq     (continuous inference, CPU clock lowered)
  4. 1 CPU core online   (continuous inference, cores 1-3 switched off)
Every setting is restored at the end, even if the script crashes
(this is a shared board).

Run as root with XRT and PYNQ environments sourced:
    python3 power_test.py
"""
import sys
import time
import numpy as np
import pynq

sys.path.insert(0, "/home/xilinx/jupyter_notebooks/tcn")
from tcn_infer import TcnDpu, load_test_data

SECONDS = 5                    # measurement time per state
CPU = "/sys/devices/system/cpu"
FREQ = f"{CPU}/cpu0/cpufreq"   # all 4 A53 cores share one clock


def read(path):
    with open(path) as f:
        return f.read().strip()


def write(path, value):
    with open(path, "w") as f:
        f.write(str(value))


rails = pynq.get_rails()
if not rails:
    sys.exit("No power rails found via pynq.get_rails() - power monitoring not available on this image.")
print("rails found:", ", ".join(rails))


def power_now():
    out = {}
    for name, rail in rails.items():
        if getattr(rail, "power", None) is not None:
            out[name] = rail.power.value
    return out


clf = TcnDpu()
X, _ = load_test_data()
results = []


def measure(label, run_dpu):
    samples, lat = [], []
    t_end = time.time() + SECONDS
    while time.time() < t_end:
        if run_dpu:
            for _ in range(50):
                t0 = time.perf_counter()
                clf.scores(X[0])
                lat.append(time.perf_counter() - t0)
        else:
            time.sleep(0.1)
        samples.append(power_now())
    avg = {k: float(np.mean([s[k] for s in samples])) for k in samples[0]}
    mean_lat = float(np.mean(lat)) * 1000 if lat else None
    results.append((label, avg, mean_lat))
    print(f"measured: {label}")


orig_max_freq = read(f"{FREQ}/scaling_max_freq")
min_freq = read(f"{FREQ}/cpuinfo_min_freq")
orig_online = {c: read(f"{CPU}/cpu{c}/online") for c in (1, 2, 3)}

try:
    measure("DPU idle", run_dpu=False)
    measure("DPU running", run_dpu=True)

    write(f"{FREQ}/scaling_max_freq", min_freq)
    time.sleep(1)
    measure(f"CPU at {int(min_freq) // 1000} MHz", run_dpu=True)
    write(f"{FREQ}/scaling_max_freq", orig_max_freq)

    for c in (1, 2, 3):
        write(f"{CPU}/cpu{c}/online", 0)
    time.sleep(1)
    measure("1 CPU core online", run_dpu=True)

finally:
    write(f"{FREQ}/scaling_max_freq", orig_max_freq)
    for c, state in orig_online.items():
        write(f"{CPU}/cpu{c}/online", state)
    print("\nboard settings restored")

names = list(results[0][1])
print(f"\n{'state':22s}" + "".join(f"{n[:12]:>13s}" for n in names) + "   latency")
for label, avg, lat in results:
    lat_s = f"{lat:.3f} ms" if lat is not None else "-"
    print(f"{label:22s}" + "".join(f"{avg[n]:12.3f}W" for n in names) + f"   {lat_s}")
