"""Compare the native zoom pipeline against SciPy using identical FIR taps."""
import argparse
import json
from pathlib import Path
import numpy as np
import scipy
from scipy.signal import zoom_fft

parser = argparse.ArgumentParser()
parser.add_argument("fixture", type=Path)
parser.add_argument("--report", type=Path, required=True)
args = parser.parse_args()
case = json.loads(args.fixture.read_text(encoding="utf-8"))
rate = case["sampleRateHz"]
factor = int(case["decimation"])
taps = np.asarray(case["taps"])
halo = (len(taps) // 2) * (factor - 1)
halo = ((halo + factor - 1) // factor) * factor
index = np.arange(case["first"] - halo, case["last"] + halo, dtype=np.int64)
iq = (0.3 * np.exp(2j * np.pi * 8040 * index / rate)
      + 0.2 * np.exp(2j * np.pi * 8080 * index / rate)
      + 0.1 * np.exp(2j * np.pi * 26000 * index / rate)).astype(np.complex64).astype(np.complex128)
iq[(index < case["viewBegin"]) | (index >= case["viewEnd"])] = 0
center = (case["frequencyLowerHz"] + case["frequencyUpperHz"]) / 2
iq *= np.exp(-2j * np.pi * center * index / rate)
for _ in range(case["stages"]):
    count = len(iq)
    iq = np.convolve(iq, taps)[len(taps) // 2:count + len(taps) // 2:2]
iq = iq[halo // factor:halo // factor + case["analysisSamples"]]
window = np.hanning(len(iq))
transformed = zoom_fft(iq * window,
    [case["frequencyLowerHz"] - center, case["frequencyUpperHz"] - center],
    m=case["points"], fs=rate / factor, endpoint=False)
expected = np.abs(transformed) ** 2 / ((rate / factor) * np.sum(window ** 2))
actual = np.asarray(case["linearPower"])
error = np.abs(actual - expected)
limit = 1e-10 + 1e-5 * expected
report = {"pass": bool(np.all(error <= limit)), "scipyVersion": scipy.__version__,
          "points": len(actual), "maximumAbsolutePowerError": float(error.max()),
          "peakBinNative": int(actual.argmax()), "peakBinScipy": int(expected.argmax()),
          "sameCoefficients": True, "realAcquisitionSamples": case["last"] - case["first"]}
args.report.write_text(json.dumps(report, indent=2), encoding="utf-8")
print(json.dumps(report, indent=2))
raise SystemExit(0 if report["pass"] else 1)
