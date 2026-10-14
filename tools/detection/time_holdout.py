"""Freeze and generate the independent M4 synthetic holdout; no detector import."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import sys
import wave

import numpy as np
import scipy
from scipy.signal import lfilter

sys.dont_write_bytecode = True
FS = 20_000_000
CENTER = 2_500_000_000.0
NFFT = 65536
BLOCK = 262144
GROUPS = (
    "pure-noise",
    "low-CNR",
    "extreme-narrow",
    "extreme-wide",
    "dense-overlap",
    "band-edges",
    "same-frequency-gaps",
    "long-duration",
    "short-1-to-8ms",
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
        encoding="utf-8",
    )


def carrier(fc, bw, intervals, cnr=20.0):
    return {
        "basebandCenterHz": float(fc),
        "nominalBandwidthHz": float(bw),
        "cnrDb": float(cnr),
        "intervalsSeconds": intervals,
        "classLabel": "SYNTH_NOISE",
    }


def parameter_grid(seed: int = 2026101404) -> list[dict]:
    """All input choices are explicit; this function does not generate IQ."""
    records = []
    bin_hz = FS / NFFT
    for group_index, name in enumerate(GROUPS):
        for index in range(32 if group_index == 8 else 8):
            duration = 0.2
            carriers = []
            if group_index == 1:
                bw = (100000, 1000000)[index % 2]
                carriers = [
                    carrier(
                        (-2500000, 2500000)[index % 2],
                        bw,
                        [[0.04, 0.16]],
                        (-5, 0, 3, 6)[index // 2],
                    )
                ]
            elif group_index == 2:
                carriers = [
                    carrier(
                        (-3000000, 3000000)[index % 2],
                        (2, 8, 32, 512)[index // 2] * bin_hz,
                        [[0.02, 0.18]],
                    )
                ]
            elif group_index == 3:
                bw = (0.1, 0.25, 0.4, 0.75)[index // 2] * FS
                fc = 0 if index % 2 == 0 else min(250000, (FS - bw) / 4)
                carriers = [carrier(fc, bw, [[0.02, 0.18]])]
            elif group_index == 4:
                count = (4, 16, 64, 128)[index // 2]
                spacing = 0.8 * FS / count
                for ordinal in range(count):
                    fc = -0.4 * FS + (ordinal + 0.5) * spacing
                    bw = 0.6 * spacing
                    if index % 2 and ordinal % 2:
                        fc -= 0.5 * spacing
                    carriers.append(carrier(fc, bw, [[0.025, 0.175]], 15))
                if index % 2:
                    carriers[0]["basebandCenterHz"] = carriers[1][
                        "basebandCenterHz"
                    ] = 0.0
                    carriers[0]["nominalBandwidthHz"] = 3 * spacing
            elif group_index == 5:
                bw = 100000
                guard = (0, 1, 4, 16)[index // 2] * bin_hz
                fc = FS / 2 - bw / 2 - guard
                carriers = [
                    carrier(fc if index % 2 else -fc, bw, [[0.02, 0.18]])
                ]
            elif group_index == 6:
                gap = (0.0005, 0.001, 0.010, 0.020)[index // 2]
                intervals = (
                    [[0.002, 0.100], [0.100 + gap, 0.198]]
                    if index % 2
                    else [[0.040, 0.060], [0.060 + gap, 0.080 + gap]]
                )
                carriers = [carrier(1200000, 100000, intervals)]
            elif group_index == 7:
                duration = (1, 2, 4, 8)[index // 2]
                intervals = (
                    [[0, duration]]
                    if index % 2 == 0
                    else [
                        [0.1 * duration, 0.45 * duration],
                        [0.55 * duration, 0.9 * duration],
                    ]
                )
                carriers = [carrier(-1700000, 200000, intervals)]
            elif group_index == 8:
                duration = 0.04
                burst = (index // 4 + 1) / 1000
                cnr = (6, 20)[(index // 2) % 2]
                start = 0.002 + (index % 2) * 0.0005
                gap = (0.001, 0.003)[index % 2]
                carriers = [
                    carrier(
                        1500000,
                        100000,
                        [
                            [start, start + burst],
                            [start + burst + gap, start + 2 * burst + gap],
                        ],
                        cnr,
                    )
                ]
            record = {
                "sampleId": f"T{group_index:02d}-{index:02d}",
                "groupIndex": group_index,
                "group": name,
                "fileIndex": index,
                "fileSeed": seed + 1000 * group_index + index,
                "sampleRateHz": FS,
                "centerFrequencyHz": CENTER,
                "durationSeconds": duration,
                "physicalSamples": str(round(duration * FS)),
                "noise": (
                    "colored-ar1-0.95"
                    if group_index == 0 and index >= 4
                    else "white-complex-unit-variance"
                ),
                "carriers": carriers,
            }
            for target in carriers:
                half = target["nominalBandwidthHz"] / 2
                assert (
                    -FS / 2
                    <= target["basebandCenterHz"] - half
                    < target["basebandCenterHz"] + half
                    <= FS / 2
                )
                target["sampleIntervals"] = [
                    [str(round(a * FS)), str(round(b * FS))]
                    for a, b in target["intervalsSeconds"]
                ]
                for a, b in target["sampleIntervals"]:
                    assert (
                        0 <= int(a) < int(b) <= int(record["physicalSamples"])
                    )
                period = max(
                    4096,
                    min(
                        524288,
                        1
                        << math.ceil(
                            math.log2(
                                max(1, 4 * FS / target["nominalBandwidthHz"])
                            )
                        ),
                    ),
                )
                target["carrierPeriodSamples"] = period
            records.append(record)
    assert len(records) == 96
    return records


def freeze_plan(root: Path, output: Path) -> dict:
    source = Path(__file__).resolve()
    protocol = (
        source.parents[2]
        / "docs/implementation/wideband-detection/M4/validation-protocol.json"
    )
    grid = parameter_grid()
    published = sum(int(record["physicalSamples"]) * 4 + 44 for record in grid)
    largest_temporary = max(
        int(record["physicalSamples"]) * 8 for record in grid
    )
    probe = root.resolve()
    while not probe.exists():
        probe = probe.parent
    required = published + largest_temporary + 512 * 1024 * 1024
    if shutil.disk_usage(probe).free < required:
        raise ValueError(
            "Insufficient disk capacity for the complete frozen grid"
        )
    plan = {
        "schema": "signalstudio-m4-holdout-generator-freeze-v1",
        "root": str(root.resolve()),
        "generator": {"path": str(source), "sha256": sha256(source)},
        "protocol": {"path": str(protocol), "sha256": sha256(protocol)},
        "seed": 2026101404,
        "prng": "NumPy-PCG64",
        "numpyVersion": np.__version__,
        "scipyVersion": scipy.__version__,
        "blockSamples": BLOCK,
        "samples": grid,
        "diskBudget": {
            "publishedWavBytes": published,
            "largestTemporaryComplex64Bytes": largest_temporary,
            "requiredFreeBytesBeforeGeneration": required,
        },
        "waveformDefinition": {
            "carrier": "periodic band-limited noise-like carrier; independent QPSK spectral coefficients followed by IFFT",
            "frequencySupport": "FFT bins inside nominal bandwidth before rectangular sample-exact activity envelope",
            "burstEnvelope": "rectangular and sample exact; frequency leakage from gating is explicitly not occupied bandwidth truth",
            "cnrDefinition": "component active-period power / expected white-noise power within nominal BW; excludes other carriers",
            "cnrCalibration": "expected thermal-band CNR, not an exact measured total-interference CNR",
            "noisePower": 1.0,
            "amplitudeScaling": "one file-wide gain giving peak complex component <=0.8 full scale",
            "pcm": "stereo little-endian PCM16 I then Q; no clipping; round nearest",
        },
        "synthetic": True,
        "realRfCapture": False,
        "independentHoldout": True,
        "generated": False,
        "predictionRead": False,
    }
    if output.exists():
        old = json.loads(output.read_text(encoding="utf-8"))
        if old != plan:
            raise ValueError(
                "Existing generator freeze differs; create an explicitly named new version"
            )
    else:
        write_json(output, plan)
    return plan


def make_period(target: dict, rng) -> tuple[np.ndarray, float]:
    period = target["carrierPeriodSamples"]
    frequency = np.fft.fftfreq(period, 1 / FS)
    keep = np.abs(frequency) <= target["nominalBandwidthHz"] / 2
    coefficients = np.zeros(period, dtype=np.complex64)
    phases = rng.integers(0, 4, int(keep.sum()))
    coefficients[keep] = np.exp(1j * (np.pi / 4 + phases * np.pi / 2)).astype(
        np.complex64
    )
    values = np.fft.ifft(coefficients).astype(np.complex64)
    values /= np.float32(
        np.sqrt(np.mean(np.abs(values.astype(np.complex128)) ** 2))
    )
    energy = np.abs(values.astype(np.complex128)) ** 2
    total = 0.0
    count = 0
    prefix = np.r_[0.0, np.cumsum(energy)]
    for a, b in target["sampleIntervals"]:
        a, b = int(a), int(b)
        length = b - a
        cycles, remainder = divmod(length, period)
        begin = a % period
        total += cycles * prefix[-1]
        first = min(remainder, period - begin)
        total += (
            prefix[begin + first] - prefix[begin] + prefix[remainder - first]
        )
        count += length
    active_power = total / count
    expected_noise_band = target["nominalBandwidthHz"] / FS
    gain = math.sqrt(
        expected_noise_band * 10 ** (target["cnrDb"] / 10) / active_power
    )
    return values, gain


def generate_one(record: dict, root: Path, block: int) -> dict:
    ident = record["sampleId"]
    wav_path = root / "data" / f"{ident}.wav"
    text_path = root / "label" / f"{ident}.txt"
    truth_path = root / "truth" / f"{ident}.json"
    for path in (wav_path, text_path, truth_path):
        if path.exists():
            raise ValueError(f"Refusing to overwrite a frozen sample: {path}")
        path.parent.mkdir(parents=True, exist_ok=True)
    temporary = root / f".{ident}.complex64.tmp"
    if temporary.exists():
        raise ValueError(
            f"Unfinished temporary sample requires explicit recovery: {temporary}"
        )
    count = int(record["physicalSamples"])
    signal = np.memmap(temporary, mode="w+", dtype="<c8", shape=(count,))
    rng = np.random.Generator(np.random.PCG64(record["fileSeed"]))
    periods = [make_period(target, rng) for target in record["carriers"]]
    colored_state = np.zeros(1, dtype=np.complex128)
    peak = 0.0
    try:
        for begin in range(0, count, block):
            end = min(count, begin + block)
            indices = np.arange(begin, end, dtype=np.int64)
            x = (
                rng.standard_normal(end - begin)
                + 1j * rng.standard_normal(end - begin)
            ) / math.sqrt(2)
            if record["noise"].startswith("colored"):
                x, colored_state = lfilter(
                    [math.sqrt(1 - 0.95**2)], [1, -0.95], x, zi=colored_state
                )
            for target, (period, gain) in zip(
                record["carriers"], periods, strict=True
            ):
                active = np.zeros(end - begin, dtype=bool)
                for a, b in target["sampleIntervals"]:
                    lo, hi = max(begin, int(a)), min(end, int(b))
                    if hi > lo:
                        active[lo - begin : hi - begin] = True
                if active.any():
                    phase = np.remainder(
                        2
                        * np.pi
                        * target["basebandCenterHz"]
                        * indices[active]
                        / FS,
                        2 * np.pi,
                    )
                    x[active] += (
                        gain
                        * period[indices[active] % len(period)]
                        * np.exp(1j * phase)
                    )
            x = x.astype(np.complex64)
            if not np.isfinite(x).all():
                raise ValueError("Nonfinite synthetic IQ")
            signal[begin:end] = x
            peak = max(
                peak, float(np.max(np.maximum(np.abs(x.real), np.abs(x.imag))))
            )
        scale = 0.8 / peak
        with wave.open(str(wav_path), "wb") as wav:
            wav.setnchannels(2)
            wav.setsampwidth(2)
            wav.setframerate(FS)
            for begin in range(0, count, block):
                x = np.array(
                    signal[begin : min(count, begin + block)], copy=True
                )
                words = (
                    np.column_stack((x.real, x.imag)).astype(np.float64)
                    * scale
                    * 32768
                )
                words = np.rint(words)
                if np.max(np.abs(words)) > 32767:
                    raise ValueError("PCM clipping forbidden")
                wav.writeframesraw(words.astype("<i2").tobytes())
        events = []
        lines = [
            f"1:\ttime_start: 0.00000\ttime_end: {count/FS:.5f}\tsig_len: {count}\n"
        ]
        for ordinal, target in enumerate(record["carriers"]):
            fc = target["basebandCenterHz"]
            bw = target["nominalBandwidthHz"]
            tokens = []
            for segment, (a, b) in enumerate(target["sampleIntervals"]):
                events.append(
                    {
                        "id": f"{ident}/carrier-{ordinal}/segment-{segment}",
                        "carrierId": f"{ident}/carrier-{ordinal}",
                        "startSample": a,
                        "endSample": b,
                        "t0": int(a) / FS,
                        "t1": int(b) / FS,
                        "f0_hz": CENTER + fc - bw / 2,
                        "f1_hz": CENTER + fc + bw / 2,
                        "frequencyFrame": "RF",
                        "frequencyQuality": "NominalLabel",
                        "classLabel": target["classLabel"],
                        "cnrDb": target["cnrDb"],
                    }
                )
                tokens.append(f"{int(a)/FS:.5f},{int(b)/FS:.5f}")
            lines.append(
                f'\t{ordinal+1}):\tmod:{target["classLabel"]}\tsys:{target["classLabel"]}\t'
                f"FC:{round(fc+FS/2)}\tBW:{round(bw)}\tFD:0\tLEN:{count}\tSPS:4.00\t"
                f'CNR:{target["cnrDb"]:.2f}\tSNR:{target["cnrDb"]:.2f}\tTIME: {" ".join(tokens)}\n'
            )
        text_path.write_text("".join(lines), encoding="utf-8")
        truth = {
            "schema": "signalstudio-m4-sample-exact-truth-v1",
            "sampleId": ident,
            "sampleRateHz": FS,
            "centerFrequencyHz": CENTER,
            "physicalSamples": str(count),
            "group": record["group"],
            "synthetic": True,
            "truthAuthority": "sample-exact",
            "waveformParameters": record,
            "pcmPeakBeforeScale": peak,
            "pcmScale": scale,
            "frequencySemantics": "nominal pre-envelope band, not measured occupied bandwidth",
            "cnrSemantics": "expected thermal-band component CNR; not total interference",
            "events": events,
        }
        write_json(truth_path, truth)
    finally:
        signal.flush()
        signal._mmap.close()
        del signal
        temporary.unlink(missing_ok=True)
    return {
        "sampleId": ident,
        "group": record["group"],
        "seed": record["fileSeed"],
        "sampleRateHz": FS,
        "centerFrequencyHz": CENTER,
        "physicalSamples": str(count),
        "truthEvents": len(events),
        "wav": {
            "path": str(wav_path.resolve()),
            "bytes": wav_path.stat().st_size,
            "sha256": sha256(wav_path),
        },
        "txtLabel": {
            "path": str(text_path.resolve()),
            "bytes": text_path.stat().st_size,
            "sha256": sha256(text_path),
        },
        "truth": {
            "path": str(truth_path.resolve()),
            "bytes": truth_path.stat().st_size,
            "sha256": sha256(truth_path),
        },
    }


def generate(freeze: Path, manifest: Path) -> dict:
    plan = json.loads(freeze.read_text(encoding="utf-8"))
    if sha256(Path(plan["generator"]["path"])) != plan["generator"]["sha256"]:
        raise ValueError("Generator changed after parameter freeze")
    if sha256(Path(plan["protocol"]["path"])) != plan["protocol"]["sha256"]:
        raise ValueError("Protocol changed after parameter freeze")
    if (
        np.__version__ != plan["numpyVersion"]
        or scipy.__version__ != plan["scipyVersion"]
    ):
        raise ValueError("PRNG environment changed after freeze")
    root = Path(plan["root"])
    root.mkdir(parents=True, exist_ok=True)
    required = plan["diskBudget"]["requiredFreeBytesBeforeGeneration"]
    if shutil.disk_usage(root).free < required:
        raise ValueError(
            f"Insufficient disk space; required free bytes {required}"
        )
    if manifest.exists():
        raise ValueError("Holdout manifest already exists")
    samples = []
    for record in plan["samples"]:
        samples.append(generate_one(record, root, plan["blockSamples"]))
        write_json(
            root / "generation-progress.json",
            {
                "complete": False,
                "samples": samples,
                "totalFiles": len(plan["samples"]),
            },
        )
        print(
            f'generated {record["sampleId"]}: {samples[-1]["truthEvents"]} truth events',
            flush=True,
        )
    report = {
        "schema": "signalstudio-m4-independent-holdout-v1",
        "baselineId": "m4-independent-synthetic-holdout-v1",
        "complete": True,
        "synthetic": True,
        "realRfCapture": False,
        "generatorFreeze": str(freeze.resolve()),
        "generatorFreezeSha256": sha256(freeze),
        "samples": samples,
        "fileCount": len(samples),
        "truthEvents": sum(v["truthEvents"] for v in samples),
        "predictionRead": False,
    }
    write_json(manifest, report)
    (root / "generation-progress.json").unlink(missing_ok=True)
    return report


def self_check() -> dict:
    grid = parameter_grid()
    assert (
        len(grid) == 96
        and sum(
            len(c["sampleIntervals"])
            for s in grid
            if s["groupIndex"] == 8
            for c in s["carriers"]
        )
        == 64
    )
    # Quantization and active-period calibration checks need no generated file.
    target = carrier(0, 100000, [[0.001, 0.002]], 20)
    target.update(
        sampleIntervals=[["20000", "40000"]], carrierPeriodSamples=4096
    )
    values, gain = make_period(target, np.random.Generator(np.random.PCG64(1)))
    expected = 100000 / FS * 100
    indices = np.arange(20000, 40000)
    assert (
        abs(
            np.mean(
                np.abs(
                    values[indices % len(values)].astype(np.complex128) * gain
                )
                ** 2
            )
            - expected
        )
        < 1e-6
    )
    return {
        "passed": True,
        "gridFiles": 96,
        "shortTruthEvents": 64,
        "calibrationDefinition": "expected white thermal-band CNR",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    plan = commands.add_parser("plan")
    plan.add_argument("--root", type=Path, required=True)
    plan.add_argument("--freeze", type=Path, required=True)
    create = commands.add_parser("generate")
    create.add_argument("--freeze", type=Path, required=True)
    create.add_argument("--manifest", type=Path, required=True)
    commands.add_parser("self-check")
    args = parser.parse_args()
    if args.command == "plan":
        result = freeze_plan(args.root, args.freeze)
        print(
            json.dumps(
                {
                    "freeze": str(args.freeze),
                    "files": len(result["samples"]),
                    "diskBudget": result["diskBudget"],
                },
                indent=2,
            )
        )
    elif args.command == "generate":
        result = generate(args.freeze, args.manifest)
        print(
            json.dumps(
                {
                    "manifest": str(args.manifest),
                    "files": result["fileCount"],
                    "truthEvents": result["truthEvents"],
                },
                indent=2,
            )
        )
    else:
        print(json.dumps(self_check(), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
