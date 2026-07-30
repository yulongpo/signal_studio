"""Regenerate the immutable MS-4.5 NumPy/SciPy numerical reference.

Run with Python 3 plus NumPy/SciPy and redirect stdout to
ms45_scipy_reference.csv. The production test never imports Python.
"""

from __future__ import annotations

import math

import numpy as np
import scipy
from scipy import signal


SAMPLE_RATE_HZ = 8_000.0
FRAME_LENGTH = 64
FFT_LENGTH = 128
HOP_LENGTH = 32
SAMPLE_COUNT = 192
SELECTED_BINS = (0, 1, 5, 11, 19, 32, 64)


def source_signal() -> np.ndarray:
    index = np.arange(SAMPLE_COUNT, dtype=np.float64)
    deterministic_dither = 0.01 * ((index.astype(np.int64) % 7) - 3)
    return (
        0.35 * np.cos(2.0 * math.pi * 5.0 * index / FRAME_LENGTH)
        + 0.20 * np.sin(2.0 * math.pi * 11.0 * index / FRAME_LENGTH)
        + deterministic_dither
    )


def one_sided_density(frame: np.ndarray, window: np.ndarray) -> np.ndarray:
    transformed = np.fft.rfft(frame * window, n=FFT_LENGTH)
    density = np.square(np.abs(transformed)) / (SAMPLE_RATE_HZ * np.sum(np.square(window)))
    density[1:-1] *= 2.0
    return density


def main() -> None:
    samples = source_signal()
    window = signal.windows.hann(FRAME_LENGTH, sym=True)
    offsets = range(0, SAMPLE_COUNT - FRAME_LENGTH + 1, HOP_LENGTH)
    rows = np.stack([one_sided_density(samples[offset : offset + FRAME_LENGTH], window) for offset in offsets])
    frequencies = np.fft.rfftfreq(FFT_LENGTH, d=1.0 / SAMPLE_RATE_HZ)

    print(f"# generator=numpy-{np.__version__};scipy-{scipy.__version__}")
    print("# window=scipy.signal.windows.hann(64,sym=True);detrend=false;scaling=density")
    print("case,row,bin,frequency_hz,time_seconds,value")
    for case, values in (("periodogram", rows[0]), ("welch", np.mean(rows, axis=0))):
        for bin_index in SELECTED_BINS:
            print(
                f"{case},-1,{bin_index},{frequencies[bin_index]:.17g},-1,"
                f"{values[bin_index]:.17g}"
            )
    for row, values in enumerate(rows):
        time_seconds = (row * HOP_LENGTH + (FRAME_LENGTH - 1) / 2.0) / SAMPLE_RATE_HZ
        for bin_index in SELECTED_BINS:
            print(
                f"stft,{row},{bin_index},{frequencies[bin_index]:.17g},"
                f"{time_seconds:.17g},{values[bin_index]:.17g}"
            )


if __name__ == "__main__":
    main()
