"""Independent NumPy FFT reference for the isolated GPU probe, never product DSP."""
import argparse
import numpy as np

p=argparse.ArgumentParser()
for name in ("input","hann","weights","output"):
    p.add_argument("--"+name,required=True)
for name in ("k","records","bins"):
    p.add_argument("--"+name,type=int,required=True)
for name in ("density-scale","divisor"):
    p.add_argument("--"+name,type=float,required=True)
a=p.parse_args()
if not (0<a.k<=2**24 and 0<a.records<=1024 and a.k*a.records<=2**28 and 0<a.bins<=2**24):
    raise ValueError("Reference bounds exceeded")
raw=np.fromfile(a.input,dtype='<c8').reshape(a.records,a.k)
hann=np.fromfile(a.hann,dtype='<f8')
weights=np.fromfile(a.weights,dtype='<f8')
if len(hann)!=a.k or len(weights)!=a.records: raise ValueError("Reference shape mismatch")
power=np.zeros(a.k,dtype=np.float64)
for record in range(a.records):
    transformed=np.fft.fft(raw[record].astype(np.complex128)*hann)
    power+=np.roll(transformed.real**2+transformed.imag**2,a.k//2)*weights[record]
if a.bins!=a.k:
    result=np.zeros(a.bins,dtype=np.float64)
    scale=a.k/a.bins
    for bin in range(a.bins):
        left,right=bin*scale,(bin+1)*scale
        for source in range(int(left),min(a.k,int(np.ceil(right)))):
            result[bin]+=power[source]*max(0.,min(right,float(source+1))-max(left,float(source)))/scale
    power=result
average=(power/a.divisor).astype(np.float32)
clamped=np.maximum(average,np.finfo(np.float32).tiny)
out=(10*np.log10(clamped.astype(np.float64)/a.density_scale)).astype('<f4')
if not np.all(np.isfinite(out)): raise ValueError("Nonfinite reference")
out.tofile(a.output)
