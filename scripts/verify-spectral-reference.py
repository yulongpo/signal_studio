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
# Explicit symmetric coefficients preserve the application's existing Hann contract.
from scipy.signal import windows
fixture = case.get("estimators")
if fixture:
    signal = np.asarray(fixture["real"]) + 1j*np.asarray(fixture["imag"])
    fs, nfft = fixture["sampleRateHz"], fixture["points"]
    scipy_windows = [windows.boxcar, windows.hann, windows.hamming, windows.blackman,
                     windows.blackmanharris, windows.flattop, lambda n: windows.kaiser(n, 8.6)]
    tapers = windows.dpss(len(signal), 3.5, Kmax=6, sym=True, norm=2)
    native_tapers = np.asarray(fixture["dpss"])
    taper_error = float(np.max(np.abs(np.abs(native_tapers @ tapers.T) - np.eye(6))))
    checks = []
    def transformed(w):
        return np.abs(np.fft.fftshift(np.fft.fft(signal*w, nfft)))**2/(fs*np.sum(w*w))
    # Independent, shrinking forward/backward-error arrays for complex Burg.
    def burg(x, order):
        ef, eb = x[1:].copy(), x[:-1].copy()
        a = np.ones(1, dtype=complex)
        variance = np.mean(np.abs(x)**2)
        for m in range(1, order+1):
            reflection = -2*np.vdot(eb, ef)/(np.vdot(ef,ef).real+np.vdot(eb,eb).real)
            if abs(reflection)>=1: reflection *= np.sqrt((1-1e-12)/abs(reflection)**2)
            a = np.r_[a,0] + reflection*np.r_[0,np.conj(a[::-1])]
            new_f, new_b = ef+reflection*eb, eb+np.conj(reflection)*ef
            ef, eb = new_f[1:], new_b[:-1]
            variance *= 1-abs(reflection)**2
        f=np.linspace(-fs/2,fs/2,nfft,endpoint=False)
        response=np.exp(-2j*np.pi*f[:,None]*np.arange(len(a))/fs)@a
        return variance/(fs*np.abs(response)**2)
    for item in fixture["cases"]:
        w = scipy_windows[item["window"]](len(signal))
        coefficient_error = float(np.max(np.abs(w-item["coefficients"])))
        expected_power = np.mean([transformed(t) for t in tapers],axis=0) if item["method"]==3 else burg(signal,4) if item["method"]==4 else transformed(w)
        error=np.abs(np.asarray(item["linearPower"])-expected_power)
        passed=bool(np.all(error <= 1e-10+expected_power*1e-5)) and coefficient_error<1e-12
        checks.append({"method":item["method"],"window":item["window"],"pass":passed,
                       "maximumAbsolutePowerError":float(error.max()),"windowError":coefficient_error})
    report["estimatorChecks"]=checks
    report["dpssOrthonormalReferenceError"]=taper_error
    report["burgReference"]="Independent complex Burg recurrence; MATLAB pburg was not executed"
    report["pass"] &= all(c["pass"] for c in checks) and taper_error<1e-7
stream = case.get("streamStatistics")
if stream:
    from scipy.signal import periodogram
    x=np.asarray(stream["real"])+1j*np.asarray(stream["imag"])
    size=stream["points"]; hop=size//2
    count=1+int(np.ceil((len(x)-size)/hop))
    powers=[]; weights=[]
    for index in range(count):
        observed=x[index*hop:index*hop+size]
        w=np.hanning(len(observed))
        if np.all(np.abs(w)<1e-15): w=np.ones(len(observed))
        _,p=periodogram(observed,stream["sampleRateHz"],window=w,nfft=size,
                        detrend=False,return_onesided=False,scaling="density")
        powers.append(np.fft.fftshift(p));weights.append(np.sum(w*w)/np.sum(np.hanning(size)**2))
    expected=[np.average(powers,axis=0,weights=weights),np.max(powers,axis=0),np.min(powers,axis=0)]
    checks=[]
    for statistic,p in enumerate(expected):
        error=np.abs(np.asarray(stream["powers"][statistic])-p)
        checks.append({"statistic":statistic,"pass":bool(np.all(error<=1e-10+p*1e-5)),
                       "maximumAbsolutePowerError":float(error.max())})
    report["streamStatistics"]=checks;report["completeStatisticalFrames"]=count
    report["pass"] &= all(c["pass"] for c in checks)
args.report.write_text(json.dumps(report, indent=2), encoding="utf-8")
print(json.dumps(report, indent=2))
raise SystemExit(0 if report["pass"] else 1)
