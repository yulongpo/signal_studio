#!/usr/bin/env python3
"""Generate deterministic smoke/golden signal files without implementing FFT.

Expected peaks are analytical. Numeric FFT/PSD validation must use the selected
oneMKL/cuFFT adapter or an approved external reference implementation.
"""
import argparse, hashlib, json, math, random, struct
from pathlib import Path

def sha256(path):
    h=hashlib.sha256()
    with path.open("rb") as f:
        for b in iter(lambda:f.read(1<<20), b""): h.update(b)
    return h.hexdigest()

def real_f32(path, frames, fs, tone):
    with path.open("wb") as f:
        for n in range(frames):
            f.write(struct.pack("<f", 0.7*math.sin(2*math.pi*tone*n/fs)))

def complex_sc16(path, frames, fs, tone, noise=0.0, seed=240722):
    rng=random.Random(seed)
    with path.open("wb") as f:
        for n in range(frames):
            p=2*math.pi*tone*n/fs
            i=max(-32768,min(32767,round(24000*math.cos(p)+noise*rng.gauss(0,1))))
            q=max(-32768,min(32767,round(24000*math.sin(p)+noise*rng.gauss(0,1))))
            f.write(struct.pack("<hh", i, q))

def main():
    ap=argparse.ArgumentParser(); ap.add_argument("--out", default="generated"); ap.add_argument("--profile", choices=["smoke","D1","D2","D3"], default="smoke"); ap.add_argument("--frames", type=int); a=ap.parse_args()
    out=Path(a.out); out.mkdir(parents=True, exist_ok=True)
    profiles={"smoke":200000,"D1":2500000000,"D2":25000000000,"D3":1250000000}; frames=a.frames or profiles[a.profile]
    if a.profile!="smoke" and a.frames is None:
        raise SystemExit("Large profiles require explicit --frames after capacity review; values are documented targets, not automatic allocations.")
    items=[]
    p=out/"D0_real_f32_10k.bin"; real_f32(p,min(frames,10000),100000,12500); items.append((p,"real",100000,12500,"float32"))
    p=out/"D0_complex_sc16.bin"; complex_sc16(p,frames,1000000,125000,120); items.append((p,"complex",1000000,125000,"int16"))
    bad=out/"D0_corrupt_tail.sc16"; bad.write_bytes((out/"D0_complex_sc16.bin").read_bytes()[:4096]+b"\x01"); items.append((bad,"complex",1000000,125000,"int16-corrupt-tail"))
    manifest={"schema":"signal.testdata/1.0","license":"CC0-1.0","profile":a.profile,"files":[]}
    for p,kind,fs,tone,dtype in items:
        manifest["files"].append({"path":p.name,"bytes":p.stat().st_size,"sha256":sha256(p),"signalKind":kind,"sampleRateHz":fs,"expectedToneHz":tone,"dtype":dtype})
    (out/"manifest.json").write_text(json.dumps(manifest,indent=2),encoding="utf-8")
if __name__=="__main__": main()
