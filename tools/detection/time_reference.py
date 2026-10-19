"""Standalone independent M4 IQ-to-events reference; never reads native heads."""

from __future__ import annotations

import argparse
from copy import deepcopy
import hashlib
import json
import math
from pathlib import Path
import shutil
import sys
import time
import wave

import numpy as np
import torch
import torch.nn.functional as F

from model_ffscn import load_model
from model_frequency_parity import associate, decode_heads, gui_frequency
from time_evaluation import require_execution_freeze, sha256

sys.dont_write_bytecode = True
NFFT = 65536
CHECKPOINT_SHA = (
    "acd997957dd85b7fc560897874b575d4776e80270d7e377d1de7559e513e40e0"
)


def write_json(path: Path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
        encoding="utf-8",
    )


def array_hash(values) -> str:
    return hashlib.sha256(np.ascontiguousarray(values).tobytes()).hexdigest()


def wav_header(path: Path):
    with wave.open(str(path), "rb") as wav:
        if (wav.getnchannels(), wav.getsampwidth(), wav.getcomptype()) != (
            2,
            2,
            "NONE",
        ):
            raise ValueError("Reference requires stereo PCM16 I/Q WAV")
        return wav.getframerate(), wav.getnframes()


def read_iq(path: Path, begin: int, end: int) -> np.ndarray:
    with wave.open(str(path), "rb") as wav:
        wav.setpos(begin)
        words = np.frombuffer(wav.readframes(end - begin), dtype="<i2")
    if words.size != 2 * (end - begin):
        raise ValueError("Truncated WAV source")
    iq = words.reshape(-1, 2).astype(np.float32) / np.float32(32768)
    return (iq[:, 0] + 1j * iq[:, 1]).astype(np.complex64)


def stft_spool(
    path: Path, output: Path, fs: int, count: int, device: str, chunk_frames=32
):
    """Contiguous bounded reads plus vectorized actual centered FFT records."""
    hop = math.floor(fs * 0.001)
    frames = count // hop
    if fs != 20000000 or frames < 10:
        raise ValueError(
            "Frozen reference domain is full-band 20 Msps and >=10 real slots"
        )
    left = max(0, NFFT - hop - count % hop) // 2
    required = frames * NFFT * 4
    if shutil.disk_usage(output.parent).free < required + 256 * 1024 * 1024:
        raise ValueError("Insufficient space for bounded reference STFT spool")
    spectra = np.memmap(output, mode="w+", dtype="<f4", shape=(frames, NFFT))
    window = torch.hann_window(
        NFFT, periodic=True, dtype=torch.float32, device=device
    )
    scale = fs * window.square().sum()
    with torch.no_grad():
        for first in range(0, frames, chunk_frames):
            after = min(frames, first + chunk_frames)
            origin = first * hop - left
            last = (after - 1) * hop - left + NFFT
            begin, end = max(0, origin), min(count, last)
            actual = read_iq(path, begin, end)
            buffer = torch.zeros(
                last - origin, dtype=torch.complex64, device=device
            )
            buffer[begin - origin : end - origin] = torch.from_numpy(actual).to(
                device
            )
            transformed = torch.fft.fftshift(
                torch.fft.fft(buffer.unfold(0, NFFT, hop) * window, dim=-1),
                dim=-1,
            )
            power = transformed.abs().square()
            density = 10 * torch.log10(
                power.clamp_min(torch.finfo(torch.float32).tiny) / scale
            )
            if not torch.isfinite(density).all():
                raise ValueError("Nonfinite reference PSD density")
            spectra[first:after] = density.cpu().numpy()
    spectra.flush()
    return spectra, hop, left


def ordered_float(raw):
    return raw ^ np.where(
        (raw & np.uint32(0x80000000)) != 0,
        np.uint32(0xFFFFFFFF),
        np.uint32(0x80000000),
    )


def radix_percentile(values: np.ndarray, percentile: float) -> float:
    """Exact ordered-float32 selection with bounded memory and NumPy 2.4 lerp."""
    flat = values.reshape(-1)
    position = (len(flat) - 1) * percentile / 100
    ranks = [math.floor(position), math.ceil(position)]
    states = [{"prefix": 0, "rank": rank} for rank in ranks]
    for shift in (24, 16, 8, 0):
        hist = [np.zeros(256, dtype=np.int64) for _ in states]
        high_mask = (
            np.uint32((0xFFFFFFFF << (shift + 8)) & 0xFFFFFFFF)
            if shift < 24
            else np.uint32(0)
        )
        for first in range(0, len(flat), 1048576):
            raw = np.ascontiguousarray(flat[first : first + 1048576]).view(
                np.uint32
            )
            order = ordered_float(raw)
            for i, state in enumerate(states):
                selected = (
                    order
                    if shift == 24
                    else order[
                        (order & high_mask) == np.uint32(state["prefix"])
                    ]
                )
                hist[i] += np.bincount(
                    ((selected >> np.uint32(shift)) & np.uint32(255)).astype(
                        np.int64
                    ),
                    minlength=256,
                )
        for state, counts in zip(states, hist, strict=True):
            cumulative = np.cumsum(counts)
            digit = int(
                np.searchsorted(cumulative, state["rank"], side="right")
            )
            before = int(cumulative[digit - 1]) if digit else 0
            state["rank"] -= before
            state["prefix"] |= digit << shift
    found = []
    for state in states:
        ordered = state["prefix"]
        bits = ordered ^ (0x80000000 if ordered & 0x80000000 else 0xFFFFFFFF)
        found.append(np.asarray([bits], dtype=np.uint32).view(np.float32)[0])
    a, b = found
    weight = np.float32(position - math.floor(position))
    difference = np.float32(b - a)
    value = (
        np.float32(a + difference * weight)
        if weight < 0.5
        else np.float32(b - difference * np.float32(1 - weight))
    )
    return float(value)


def infer_windows(spectra, model, device, fs, count, hop, left, output):
    frames = len(spectra)
    starts = list(range(0, frames - 10 + 1, 5))
    tail = frames - 10
    if starts[-1] != tail:
        starts.append(tail)
    windows = []
    parameters = {"scoreThreshold": 0.7, "nmsIou": 0.3, "topK": 512}
    first_hz = -fs / 2
    for ordinal in range(0, len(starts), 2):
        batch = starts[ordinal : ordinal + 2]
        tensor = torch.from_numpy(
            np.stack([np.asarray(spectra[s : s + 10]) for s in batch])
        ).to(device)
        mean = tensor.mean(dim=(1, 2), keepdim=True)
        deviation = tensor.std(
            dim=(1, 2), correction=1, keepdim=True
        ).clamp_min(torch.finfo(torch.float32).eps)
        normalized = ((tensor - mean) / deviation).unsqueeze(1)
        with torch.no_grad():
            heads = model(normalized)
        native = [head.detach().cpu().numpy() for head in heads]
        for member, start in enumerate(batch):
            raw = np.stack([head[member, 0, 0] for head in native]).astype(
                "<f4"
            )
            if not np.isfinite(raw).all():
                raise ValueError("Nonfinite reference model output")
            candidates = decode_heads(raw, parameters, NFFT, False, True)
            mapping = {
                "windowId": f"reference-frame-{start}",
                "firstFrameIndex": str(start),
                "frequencyFirstBinHz": first_hz,
                "frequencyStepHz": fs / NFFT,
                "nominalStartSample": str(start * hop),
                "nominalEndSample": str((start + 10) * hop),
                "contextStartSample": str(max(0, start * hop - left)),
                "contextEndSample": str(
                    min(count, (start + 9) * hop - left + NFFT)
                ),
                "frames": [
                    {
                        "frameIndex": str(frame),
                        "startSample": str(max(0, frame * hop - left)),
                        "endSample": str(min(count, frame * hop - left + NFFT)),
                    }
                    for frame in range(start, start + 10)
                ],
                "inputNormalizedSha256": array_hash(
                    normalized[member].detach().cpu().numpy()
                ),
                "headSha256": array_hash(raw),
            }
            bin_path = output / "windows" / f"{start:08d}.heads.f32"
            bin_path.parent.mkdir(parents=True, exist_ok=True)
            raw.tofile(bin_path)
            write_json(
                bin_path.with_suffix(".json"),
                {
                    "mapping": mapping,
                    "candidates": candidates,
                    "heads": {
                        "path": str(bin_path),
                        "sha256": sha256(bin_path),
                        "shape": [3, 16384],
                    },
                },
            )
            windows.append((mapping, candidates))
        print(
            f"reference model windows {min(ordinal+2,len(starts))}/{len(starts)}",
            flush=True,
        )
    return windows


def fine_runs(
    mask: np.ndarray, min_on: int, min_off: int, split=True
) -> list[tuple[int, int]]:
    differences = np.diff(
        np.r_[False, np.asarray(mask, dtype=bool), False].astype(np.int8)
    )
    starts = np.flatnonzero(differences == 1)
    ends = np.flatnonzero(differences == -1)
    runs = [(int(a), int(b)) for a, b in zip(starts, ends) if b - a >= min_on]
    merged = []
    for a, b in runs:
        if merged and a - merged[-1][1] < min_off:
            merged[-1] = (merged[-1][0], b)
        else:
            merged.append((a, b))
    if not split and merged:
        merged = [(merged[0][0], merged[-1][1])]
    return merged


def envelope_power(iq, fs, fc, bw, device):
    x = torch.from_numpy(np.asarray(iq, dtype=np.complex64)).to(device)
    indices = torch.arange(len(iq), dtype=torch.float64, device=device)
    phase = torch.remainder(-2 * np.pi * fc * indices / fs, 2 * np.pi).float()
    base = x * torch.polar(torch.ones_like(phase), phase)
    frequency = torch.fft.fftfreq(len(iq), 1 / fs, device=device).abs()
    cutoff = bw / 2
    taper = max(bw * 0.25, 1.0)
    response = torch.ones_like(frequency)
    response[frequency > cutoff + taper] = 0
    transition = (frequency > cutoff) & (frequency <= cutoff + taper)
    response[transition] = 0.5 * (
        1 + torch.cos(np.pi * (frequency[transition] - cutoff) / taper)
    )
    filtered = torch.fft.ifft(torch.fft.fft(base) * response)
    return (filtered.abs().square()).float()


def sample_refine(
    block, path, fs, count, parameters, device, output, budget, semantics
):
    a = max(0, int((block["t0"] - 0.008) * fs))
    b = min(count, int((block["t1"] + 0.008) * fs))
    evidence = {
        "method": "sample",
        "supportStartSample": str(a),
        "supportEndSample": str(b),
        "filterSupportSamples": str(max(0, b - a)),
        "smoothingNumericalVersion": "frozen-torch-float32-cumsum",
        "continuousTrack": block["coverage"] >= 0.7,
        "mathematicalSemantics": semantics,
        "fineParametersApplied": True,
        "coarseParametersApplied": False,
    }
    if b <= a or (b - a) * 64 > budget:
        return [], {
            **evidence,
            "status": "fallback",
            "reason": "reference-exact-roi-working-budget-exceeded",
        }
    power = envelope_power(
        read_iq(path, a, b), fs, block["fc_hz"], block["bw_hz"], device
    )
    win = max(1, int(parameters["smoothSeconds"] * fs))
    pad = F.pad(power, (win - 1, 0))
    cumulative = F.pad(torch.cumsum(pad, dim=-1), (1, 0))
    smoothed = ((cumulative[win:] - cumulative[:-win]) / win).cpu().numpy()
    power_cpu = power.cpu().numpy()
    noise = float(np.percentile(smoothed, 0.5))
    burst = float(np.median(smoothed))
    frac = (
        (
            0.5 * (parameters["sampleEdgeRatio"] / 0.6)
            if semantics == "product-v2"
            else 0.5
        )
        if block["coverage"] >= 0.7
        else parameters["sampleEdgeRatio"]
    )
    threshold = noise + frac * (burst - noise)
    mask = smoothed > threshold
    # Measure the declared native f64 accumulator difference without replacing
    # the frozen reference cumsum or relaxing its tolerance.
    f64prefix = np.r_[0.0, np.cumsum(power_cpu, dtype=np.float64)]
    previous = np.maximum(0, np.arange(1, len(power_cpu) + 1) - win)
    rolling = ((f64prefix[1:] - f64prefix[previous]) / win).astype(np.float32)
    rolling_noise = float(np.percentile(rolling, 0.5))
    rolling_burst = float(np.median(rolling))
    rolling_threshold = rolling_noise + frac * (rolling_burst - rolling_noise)
    limits = 1e-7 + 1e-3 * np.abs(smoothed)
    difference = np.abs(rolling.astype(np.float64) - smoothed)
    min_on = max(2, int(max(0.0005, parameters["minOnSeconds"]) * fs))
    min_off = max(2, int(parameters["minOffSeconds"] * fs))
    user_no_split = not parameters["splitBursts"]
    if semantics == "product-v2" and user_no_split:
        min_on = max(2, int(0.0005 * fs))
        min_off = 2
    split = parameters["splitBursts"] and block["coverage"] < 0.7
    runs = fine_runs(mask, min_on, min_off, split)
    rolling_runs = fine_runs(
        rolling > rolling_threshold, min_on, min_off, split
    )
    evidence.update(
        noise=noise,
        burst=burst,
        threshold=threshold,
        comparisonThreshold=float(np.float32(threshold)),
        effectiveEdgeFraction=frac,
        smoothingSamples=str(win),
        minOnSamples=str(min_on),
        minOffSamples=str(min_off),
        durationParametersApplied=not (
            semantics == "product-v2" and user_no_split
        ),
        onFraction=float(mask.mean()),
        powerSha256=array_hash(power_cpu),
        smoothedSha256=array_hash(smoothed),
        maskSha256=array_hash(np.packbits(mask)),
        integerRuns=[list(run) for run in runs],
        nativeRollingNumericalDiagnostic={
            "maxAbs": float(difference.max()),
            "overFrozenToleranceCount": int((difference > limits).sum()),
            "differentMaskSamplesUsingOwnThreshold": int(
                np.count_nonzero(mask != (rolling > rolling_threshold))
            ),
            "differentMaskSamplesUsingReferenceThreshold": int(
                np.count_nonzero(mask != (rolling > threshold))
            ),
            "rollingNoise": rolling_noise,
            "rollingBurst": rolling_burst,
            "rollingThreshold": rolling_threshold,
            "integerRuns": list(map(list, rolling_runs)),
            "runsExactlyMatch": runs == rolling_runs,
        },
    )
    # A bounded trace preserves physical bucket endpoints, not display pixels.
    step = max(1, math.ceil(len(smoothed) / 4096))
    evidence["trace"] = [
        {
            "startSample": str(a + i),
            "endSample": str(min(b, a + i + step)),
            "smoothedMean": float(
                np.mean(smoothed[i : i + step], dtype=np.float64)
            ),
        }
        for i in range(0, len(smoothed), step)
    ]
    reason = ""
    if burst <= noise * 1.3:
        reason = "sample-burst-noise-ratio-too-low"
    elif mask.mean() < 0.03:
        reason = "sample-on-fraction-too-low"
    elif not runs:
        reason = "sample-no-sustained-on-run"
    if reason:
        return [], {**evidence, "status": "fallback", "reason": reason}
    result = []
    for first, end in runs:
        nt0 = max(0.0, (a + first) / fs)
        nt1 = min(
            count // int(fs * 0.001) * int(fs * 0.001) / fs, (a + end) / fs
        )
        if nt1 > nt0:
            result.append(
                {
                    **block,
                    "t0": nt0,
                    "t1": nt1,
                    "startSample": str(a + first),
                    "endSample": str(min(round(nt1 * fs), count)),
                }
            )
    return result, {
        **evidence,
        "status": "applied" if result else "fallback",
        "reason": "" if result else "sample-runs-outside-publish-bounds",
    }


def roi_profiles(
    crop,
    edge_ratio=0.6,
    smooth_seconds=0.001,
    semantics="gui-frozen",
    device="cpu",
):
    x = F.interpolate(
        torch.as_tensor(
            np.asarray(crop).copy(), dtype=torch.float32, device=device
        ).T[None, None],
        size=(512, 64),
        mode="bilinear",
        align_corners=False,
    )[0, 0]
    for _ in range(3):
        x = x - x.mean()
    x = F.relu(x)
    positive = x > 0
    fraction = float(positive.float().mean())
    average = float(x[positive].mean()) if positive.any() else 0.0
    factor = edge_ratio / 0.6 if semantics == "product-v2" else 1.0
    kernel = (
        min(63, max(1, 2 * math.floor((5 * smooth_seconds / 0.001) / 2) + 1))
        if semantics == "product-v2"
        else 5
    )
    time_mask = (
        F.avg_pool1d(
            (x.mean(0) > (1 - fraction) / 2 * average * factor).float()[
                None, None
            ],
            kernel,
            1,
            kernel // 2,
        )[0, 0]
        > 0.5
    )
    frequency_mask = (
        F.avg_pool1d(
            (x.mean(1) > (1 - fraction) / 4 * average * factor).float()[
                None, None
            ],
            kernel,
            1,
            kernel // 2,
        )[0, 0]
        > 0.5
    )
    return (
        time_mask.cpu().numpy(),
        frequency_mask.cpu().numpy(),
        {
            "positiveFraction": fraction,
            "positiveMean": average,
            "kernel": kernel,
            "thresholdFactor": factor,
            "pooledSha256": array_hash(x.cpu().numpy()),
        },
    )


def roi_refine(block, spectra, fs, hop, parameters, semantics, device="cpu"):
    frames, width = spectra.shape
    first_hz = -fs / 2
    step = fs / width
    centres = first_hz + np.arange(width) * step
    fa, fb = np.clip(
        np.searchsorted(centres, [block["f0_hz"], block["f1_hz"]]), 0, width - 1
    ).astype(int)
    ta = max(0, min(frames - 1, math.ceil(block["t0"] * fs / hop)))
    tb = max(0, min(frames - 1, math.ceil(block["t1"] * fs / hop) - 1))
    if tb < ta:
        ta, tb = tb, ta
    mask, fmask, diagnostics = roi_profiles(
        spectra[ta : tb + 1, fa : fb + 1],
        parameters["sampleEdgeRatio"],
        parameters["smoothSeconds"],
        semantics,
        device,
    )
    span = tb - ta + 1
    col_dt = span * hop / fs / 64
    min_on = max(1, math.ceil(parameters["minOnSeconds"] / col_dt))
    min_off = max(1, math.ceil(parameters["minOffSeconds"] / col_dt))
    user_no_split = not parameters["splitBursts"]
    if semantics == "product-v2" and user_no_split:
        min_on = min_off = 1
    split = parameters["splitBursts"] and block["coverage"] < 0.7
    runs = fine_runs(mask, min_on, min_off, split)
    nonzero = np.flatnonzero(fmask)
    fo, ft = (int(nonzero[0]), int(nonzero[-1])) if len(nonzero) else (0, 511)
    nfa = max(fa, min(fb, fa + math.floor(fo * (fb - fa + 1) / 512)))
    nfb = max(fa, min(fb, fa + math.ceil((ft + 1) * (fb - fa + 1) / 512) - 1))
    result = []
    for a, b in runs:
        a = max(ta, min(tb, ta + math.floor(a * span / 64)))
        c = max(ta, min(tb, ta + math.ceil(b * span / 64) - 1))
        if c - a < 2:
            continue
        low = centres[nfa] - 0.5 * step
        high = centres[nfb] + 0.5 * step
        result.append(
            {
                **block,
                "t0": a * hop / fs,
                "t1": (c + 1) * hop / fs,
                "startSample": str(a * hop),
                "endSample": str((c + 1) * hop),
                "f0_hz": float(low),
                "f1_hz": float(high),
                "fc_hz": float((low + high) / 2),
                "bw_hz": float(high - low),
            }
        )
    return result, {
        "method": "roi",
        "status": "applied" if result else "fallback",
        "reason": "" if result else "roi-no-run-maps-to-three-real-frames",
        "sourceFrameFirst": str(ta),
        "sourceFrameLast": str(tb),
        "physicalColumnSeconds": col_dt,
        "minOnCells": min_on,
        "minOffCells": min_off,
        "continuousTrack": block["coverage"] >= 0.7,
        "durationParametersApplied": not (
            semantics == "product-v2" and user_no_split
        ),
        "fineParametersApplied": True,
        "coarseParametersApplied": False,
        "mathematicalSemantics": semantics,
        **diagnostics,
    }


def coarse_refine(block, spectra, fs, hop, parameters):
    width = spectra.shape[1]
    step = fs / width
    centres = -fs / 2 + np.arange(width) * step
    fa, fb = np.clip(
        np.searchsorted(centres, [block["f0_hz"], block["f1_hz"]]), 0, width - 1
    ).astype(int)
    ta = max(0, min(len(spectra) - 1, math.ceil(block["t0"] * fs / hop)))
    tb = max(0, min(len(spectra) - 1, math.ceil(block["t1"] * fs / hop) - 1))
    band = np.asarray(spectra[ta : tb + 1, fa : fb + 1])
    top = max(1, math.ceil(0.05 * band.shape[1]))
    profile = np.partition(band, -top, axis=1)[:, -top:].mean(axis=1)
    floor = float(np.percentile(profile, 20))
    threshold = floor + 6
    mask = profile > threshold
    result = []
    if mask.any() and not mask.all():
        if not parameters["splitBursts"]:
            idx = np.flatnonzero(mask)
            runs = [(int(idx[0]), int(idx[-1]) + 1)]
        else:
            raw = fine_runs(mask, 1, 1, True)
            merged = []
            gap = max(
                1, math.ceil(parameters["coarseSplitGapSeconds"] * fs / hop)
            )
            for a, b in raw:
                if merged and a - merged[-1][1] < gap:
                    merged[-1] = (merged[-1][0], b)
                else:
                    merged.append((a, b))
            minimum = max(
                1, math.ceil(parameters["coarseMinBurstSeconds"] * fs / hop)
            )
            runs = [(a, b) for a, b in merged if b - a >= minimum]
        for a, b in runs:
            if b - a < 3:
                continue
            result.append(
                {
                    **block,
                    "t0": (ta + a) * hop / fs,
                    "t1": (ta + b) * hop / fs,
                    "startSample": str((ta + a) * hop),
                    "endSample": str((ta + b) * hop),
                }
            )
    return result, {
        "method": "coarse",
        "status": "applied" if result else "fallback",
        "reason": "" if result else "coarse-no-valid-distinct-on-mask",
        "noise": floor,
        "threshold": threshold,
        "coarseParametersApplied": parameters["splitBursts"],
        "fineParametersApplied": False,
    }


def deduplicate(blocks, bin_hz):
    """Collapse overlap/touch only; never bridge a genuine positive gap."""
    result = deepcopy(blocks)
    while True:
        choices = []
        for i, a in enumerate(result):
            for j in range(i + 1, len(result)):
                b = result[j]
                gap = max(a["t0"], b["t0"]) - min(a["t1"], b["t1"])
                inter = min(a["f1_hz"], b["f1_hz"]) - max(
                    a["f0_hz"], b["f0_hz"]
                )
                if gap > 1e-12 or inter <= 0:
                    continue
                aw = a["f1_hz"] - a["f0_hz"]
                bw = b["f1_hz"] - b["f0_hz"]
                iou = inter / (aw + bw - inter)
                contain = inter / min(aw, bw)
                distance = abs(
                    (a["f0_hz"] + a["f1_hz"] - b["f0_hz"] - b["f1_hz"]) / 2
                )
                if iou >= 0.3 or (
                    contain >= 0.3 and distance <= min(aw, bw) / 2 + 2 * bin_hz
                ):
                    choices.append(
                        (
                            (
                                max(0, gap),
                                -max(iou, contain),
                                distance,
                                min(a["t0"], b["t0"]),
                                i,
                                j,
                            ),
                            i,
                            j,
                        )
                    )
        if not choices:
            break
        _, i, j = min(choices)
        a = result[i]
        b = result.pop(j)
        a.update(
            t0=min(a["t0"], b["t0"]),
            t1=max(a["t1"], b["t1"]),
            f0_hz=min(a["f0_hz"], b["f0_hz"]),
            f1_hz=max(a["f1_hz"], b["f1_hz"]),
        )
        for key, score in b["windowScores"].items():
            a["windowScores"][key] = max(a["windowScores"].get(key, 0), score)
        a["sourceTrackIds"] = sorted(
            set(a["sourceTrackIds"] + b["sourceTrackIds"])
        )
        a["refineStatus"] = (
            "fallback"
            if "fallback" in (a["refineStatus"], b["refineStatus"])
            else "applied"
        )
    return sorted(
        result, key=lambda b: (b["f0_hz"], b["t0"], b["f1_hz"], b["id"])
    )


def run(args) -> dict:
    require_execution_freeze(args.freeze, Path(__file__))
    if sha256(args.checkpoint) != CHECKPOINT_SHA:
        raise ValueError("Frozen epoch1200 checkpoint changed")
    args.output.mkdir(parents=True, exist_ok=True)
    result_path = args.output / "result.json"
    if result_path.exists():
        raise ValueError(
            "Reference result already exists; do not overwrite an experiment"
        )
    started = time.monotonic()
    fs, count = wav_header(args.file)
    metadata = {
        "schema": "signalstudio-m4-independent-time-reference-v1",
        "sourcePath": str(args.file.resolve()),
        "sourceSha256": sha256(args.file),
        "sampleRateHz": fs,
        "centerFrequencyHz": args.center,
        "physicalSamples": str(count),
        "strategy": args.strategy,
        "shortProfile": args.short,
        "semantics": args.semantics,
        "checkpointSha256": CHECKPOINT_SHA,
        "referenceSha256": sha256(Path(__file__)),
        "executionFreezeSha256": sha256(args.freeze),
        "device": args.device,
        "torch": torch.__version__,
        "numpy": np.__version__,
        "sourceRateEqualsAnalysisRate": True,
        "fullEffectiveBandwidth": True,
        "nativeHeadRead": False,
        "externalBusinessImported": False,
        "referenceWorkingBudgetBytes": args.sample_budget_mib * 1024 * 1024,
    }
    metadata["profileId"] = "ffscn-gui-iq-v1"
    metadata["profileVersion"] = 1
    metadata["experimentProfile"] = (
        "ffscn-gui-iq-v1-short-burst-m4-v1" if args.short else "ffscn-gui-iq-v1"
    )
    if args.semantics == "product-v2":
        metadata["parameterExtensionVersion"] = (
            "signalstudio-refinement-m0p-parameters-v2"
        )
    spool = args.output / "stft-working.f32"
    spectra = None
    try:
        torch.backends.cuda.matmul.allow_tf32 = False
        torch.backends.cudnn.allow_tf32 = False
        torch.set_num_threads(max(1, min(8, torch.get_num_threads())))
        spectra, hop, left = stft_spool(
            args.file, spool, fs, count, args.device
        )
        model = load_model(args.checkpoint, args.device)
        windows = infer_windows(
            spectra, model, args.device, fs, count, hop, left, args.output
        )
        del model
        if args.device.startswith("cuda"):
            torch.cuda.empty_cache()
        parameters = {
            "scoreThreshold": 0.7,
            "nmsIou": 0.3,
            "topK": 512,
            "batchSize": 2,
            "minHits": 2,
            "maxMiss": 1,
            "mergeFrequencyHz": 100000,
            "bandwidthMarginDb": 6,
            "splitBursts": not args.strategy.endswith("no-split"),
            "timeStrategy": (
                "sample"
                if args.strategy == "no-split"
                else args.strategy.removesuffix("-no-split")
            ),
            "minOnSeconds": 0.0005 if args.short else 0.005,
            "minOffSeconds": 0.001 if args.short else 0.010,
            "smoothSeconds": (
                (0.0001 if args.short else 0.001)
                if args.smooth_ms is None
                else args.smooth_ms / 1000
            ),
            "sampleEdgeRatio": args.edge_ratio,
            "coarseSplitGapSeconds": 0.020,
            "coarseMinBurstSeconds": 0.010,
        }
        request = {
            "parameters": parameters,
            "sourceIdentity": {"sampleRateHz": fs},
        }
        tracks = associate(windows, request, len(windows))
        floor = radix_percentile(spectra, 10)
        events = []
        states = []
        observations = []
        for ordinal, track in enumerate(tracks):
            first = max(
                0, min(len(spectra) - 1, math.ceil(track.nominal[0] / hop))
            )
            after = max(
                first + 1, min(len(spectra), math.ceil(track.nominal[1] / hop))
            )
            frequency = gui_frequency(
                spectra[first:after].mean(axis=0),
                floor,
                (track.low, track.high),
                -fs / 2,
                fs / NFFT,
                6,
            )
            block = {
                "id": f"reference-track-{ordinal}",
                "t0": track.nominal[0] / fs,
                "t1": track.nominal[1] / fs,
                "f0_hz": frequency["lowerHz"],
                "f1_hz": frequency["upperHz"],
                "fc_hz": (frequency["lowerHz"] + frequency["upperHz"]) / 2,
                "bw_hz": frequency["upperHz"] - frequency["lowerHz"],
                "coverage": len(track.windows) / len(windows),
                "windowScores": {
                    key: item["score"] for key, item in track.windows.items()
                },
                "sourceTrackIds": [f"reference-track-{ordinal}"],
                "frequencyAnalysis": frequency,
                "legacyDuplicateWeightedScoreMean": track.summary()[
                    "legacyDuplicateWeightedScoreMean"
                ],
            }
            observations.append(deepcopy(block))
            method = parameters["timeStrategy"]
            if method == "sample":
                refined, state = sample_refine(
                    block,
                    args.file,
                    fs,
                    count,
                    parameters,
                    args.device,
                    args.output,
                    args.sample_budget_mib * 1024 * 1024,
                    args.semantics,
                )
            elif method == "roi":
                refined, state = roi_refine(
                    block,
                    spectra,
                    fs,
                    hop,
                    parameters,
                    args.semantics,
                    args.device,
                )
            elif method == "coarse":
                refined, state = coarse_refine(
                    block, spectra, fs, hop, parameters
                )
            else:
                refined, state = [], {
                    "method": "none",
                    "status": "unknown",
                    "reason": "time-strategy-none",
                }
            state["trackId"] = block["id"]
            write_json(
                args.output / "refinement" / f"{ordinal:05d}.json", state
            )
            states.append(
                {key: value for key, value in state.items() if key != "trace"}
            )
            if not refined and method != "none":
                refined = [block]  # preserve GUI fallback parent span
            for segment, child in enumerate(refined):
                child = {
                    **child,
                    "id": f'{block["id"]}/activity-{segment}',
                    "refineStatus": state["status"],
                    "refineMethod": method,
                    "fallbackReason": state["reason"],
                    "frequencyFrame": "Baseband",
                }
                events.append(child)
            print(
                f'reference refine {ordinal+1}/{len(tracks)}: {state["status"]}',
                flush=True,
            )
        events = deduplicate(events, fs / NFFT)
        for event in events:
            event["startSample"] = str(round(event["t0"] * fs))
            event["endSample"] = str(round(event["t1"] * fs))
            event["f0_hz"] += args.center
            event["f1_hz"] += args.center
            event["fc_hz"] += args.center
            event["frequencyFrame"] = "RF"
            event["scoreMean"] = float(
                np.mean(list(event["windowScores"].values()))
            )
            event["scoreMax"] = max(event["windowScores"].values())
            event["hits"] = len(event["windowScores"])
            event["coverage"] = event["hits"] / len(windows)
        report = {
            **metadata,
            "pass": True,
            "events": events,
            "observations": observations,
            "trackCount": len(tracks),
            "unknownObservations": (
                len(tracks) if parameters["timeStrategy"] == "none" else 0
            ),
            "refinementStates": states,
            "modelWindows": len(windows),
            "uniqueFrames": len(spectra),
            "sourceLeadingPadding": str(left),
            "noiseFloorDb": floor,
            "parameters": parameters,
            "elapsedSeconds": time.monotonic() - started,
        }
    except Exception as error:
        report = {
            **metadata,
            "pass": False,
            "events": [],
            "error": f"{type(error).__name__}: {error}",
            "elapsedSeconds": time.monotonic() - started,
        }
    finally:
        if spectra is not None:
            spectra.flush()
            spectra._mmap.close()
            del spectra
        if not args.retain_stft:
            spool.unlink(missing_ok=True)
    write_json(result_path, report)
    return report


def self_check() -> dict:
    # Merge-after-delete, equality and continuous/noSplit are discrete checks.
    mask = np.array([0, 1, 1, 0, 1, 1, 1, 0, 0], dtype=bool)
    assert fine_runs(mask, 3, 2) == [(4, 7)]
    assert fine_runs(mask, 2, 1) == [(1, 3), (4, 7)]
    assert fine_runs(mask, 2, 2) == [(1, 7)]
    assert fine_runs(mask, 2, 1, False) == [(1, 7)]
    rng = np.random.Generator(np.random.PCG64(7))
    values = rng.normal(size=(13, 17)).astype(np.float32)
    assert radix_percentile(values, 10) == float(np.percentile(values, 10))
    x = np.exp(2j * np.pi * 1000 * np.arange(2048) / 8192).astype(np.complex64)
    power = envelope_power(x, 8192, 1000, 128, "cpu").numpy()
    assert np.max(np.abs(power - 1)) < 1e-4
    mask, fmask, _ = roi_profiles(np.full((3, 8), -50, dtype=np.float32))
    assert not mask.any() and not fmask.any()
    return {
        "passed": True,
        "checks": [
            "fine-run-order-and-equality",
            "exact-bounded-float32-percentile",
            "relative-phase-zero-phase-lowpass",
            "constant-roi-no-mask",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--file", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--checkpoint",
        type=Path,
        default=Path(
            "D:/project/FFSCN_train2/FFSCN_TEST_GUI/config/epoch_1200.pth"
        ),
    )
    parser.add_argument("--freeze", type=Path)
    parser.add_argument("--device", default="cuda:0")
    parser.add_argument(
        "--strategy",
        choices=(
            "sample",
            "roi",
            "coarse",
            "none",
            "no-split",
            "roi-no-split",
            "coarse-no-split",
        ),
        default="sample",
    )
    parser.add_argument("--short", action="store_true")
    parser.add_argument(
        "--semantics",
        choices=("gui-frozen", "product-v2"),
        default="gui-frozen",
    )
    parser.add_argument("--edge-ratio", type=float, default=0.6)
    parser.add_argument("--smooth-ms", type=float)
    parser.add_argument("--center", type=float, default=2.5e9)
    parser.add_argument("--sample-budget-mib", type=int, default=2048)
    parser.add_argument("--retain-stft", action="store_true")
    parser.add_argument("--self-check", action="store_true")
    args = parser.parse_args()
    if args.self_check:
        print(json.dumps(self_check(), indent=2))
        return 0
    if not all((args.file, args.output, args.freeze)):
        parser.error("--file, --output and --freeze are required")
    if (
        not 0 < args.edge_ratio < 1
        or args.smooth_ms is not None
        and not 0 <= args.smooth_ms <= 60000
    ):
        parser.error("Invalid refinement parameter")
    if not 1 <= args.sample_budget_mib <= 4096:
        parser.error("Sample working budget must be 1..4096 MiB")
    result = run(args)
    print(
        json.dumps(
            {
                "pass": result["pass"],
                "result": str(args.output / "result.json"),
                "events": len(result["events"]),
                "error": result.get("error"),
            },
            indent=2,
        )
    )
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
