"""Regenerate the bounded 20 Msps IQ/PSD golden with independent CPU formulas.

No reference business module or model checkpoint is imported. Only actual WAV
samples, periodic Hann, Torch CPU FFT and a ddof=1 z-score are evaluated. The
source path and full SHA-256 are taken from the reviewed M0 asset manifest.
"""

import argparse
import hashlib
import json
import math
import wave
from pathlib import Path

import numpy as np
import torch


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def frame_density(path, index, count, hop, fft_size, left_padding, window, fs):
    """Read at most one FFT record; preserve original sample indices."""
    origin = index * hop - left_padding
    begin, end = max(0, origin), min(count, origin + fft_size)
    with wave.open(str(path), "rb") as wav:
        wav.setpos(begin)
        words = np.frombuffer(wav.readframes(end - begin), dtype="<i2")
    iq = words.reshape(-1, 2).astype(np.float32) / np.float32(32768)
    signal = torch.zeros(fft_size, dtype=torch.complex64, device="cpu")
    first = begin - origin
    signal[first : first + len(iq)] = torch.from_numpy(iq[:, 0].copy()) + (
        1j * torch.from_numpy(iq[:, 1].copy())
    )
    transformed = torch.fft.fftshift(torch.fft.fft(signal * window))
    power = transformed.abs().square()
    density = power.clamp_min(torch.finfo(torch.float32).tiny) / (
        fs * window.square().sum()
    )
    return 10 * torch.log10(density)


def generate(assets_path, output_path):
    assets = json.loads(assets_path.read_text(encoding="utf-8"))
    identity = assets["datasets"]["matlabGenerator"]["goldenPair"]["wav"]
    path = Path(identity["path"])
    if file_sha256(path) != identity["sha256"]:
        raise ValueError("WAV source identity does not match the M0 manifest")
    with wave.open(str(path), "rb") as wav:
        if wav.getnchannels() != 2 or wav.getsampwidth() != 2:
            raise ValueError("The golden requires actual stereo PCM16 I/Q")
        fs, count = wav.getframerate(), wav.getnframes()
    fft_size = 65536
    hop = math.floor(fs * 0.001)
    frames = count // hop
    padding = max(0, fft_size - hop - count % hop)
    left, right = padding // 2, padding - padding // 2
    bins = np.linspace(0, fft_size - 1, 128, dtype=np.int64).tolist()
    window = torch.hann_window(fft_size, periodic=True, dtype=torch.float32)
    result = {
        "schema": "signalstudio-detection-source-golden-v1",
        "oracle": "Independent Torch CPU float32 FFT and explicit frozen profile "
        "formulas; no external business implementation imported",
        "torchVersion": torch.__version__,
        "wavSha256": identity["sha256"],
        "sampleRateHz": fs,
        "sampleCount": str(count),
        "fftSize": fft_size,
        "hopSamples": str(hop),
        "sourceLeadingPadding": str(left),
        "sourceTrailingPadding": str(right),
        "frameCount": str(frames),
        "frequencyFirstBinHz": -fs / 2,
        "frequencyStepHz": fs / fft_size,
        "sampledBins": bins,
        "frames": [],
        "windows": [],
        "psdToleranceDb": 0.02,
        "normalizationTolerance": 0.002,
    }
    for index in (0, 1, 397, 799):
        spectrum = frame_density(path, index, count, hop, fft_size, left, window, fs)
        result["frames"].append(
            {
                "frameIndex": str(index),
                "sampledDensityDb": [float(spectrum[bin_index]) for bin_index in bins],
            }
        )
    for first in (0, 395, 790):
        spectra = torch.stack(
            [
                frame_density(path, i, count, hop, fft_size, left, window, fs)
                for i in range(first, first + 10)
            ]
        )
        mean, std = spectra.mean(), spectra.std(correction=1)
        normalized = (spectra - mean) / std.clamp_min(torch.finfo(torch.float32).eps)
        result["windows"].append(
            {
                "firstFrameIndex": str(first),
                "meanDb": float(mean),
                "sampleStdDb": float(std),
                "sampledTensor": [
                    [row, bin_index, float(normalized[row, bin_index])]
                    for row in (0, 4, 9)
                    for bin_index in bins[::4]
                ],
            }
        )
    output_path.write_bytes(
        (json.dumps(result, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    )
    print(
        json.dumps(
            {"output": str(output_path), "sourceSha256": identity["sha256"]},
            ensure_ascii=False,
        )
    )


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--assets", type=Path,
        default=root / "docs/implementation/wideband-detection/M0/assets.json",
    )
    parser.add_argument(
        "--output", type=Path,
        default=Path(__file__).with_name("detection-source-20m-golden.json"),
    )
    arguments = parser.parse_args()
    generate(arguments.assets, arguments.output)
