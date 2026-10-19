"""Same-source M4 sample energy diagnostics; never adjusts frozen tolerances."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import sys
import time

sys.dont_write_bytecode = True

import numpy as np
import torch
import torch.nn.functional as F

from time_evaluation import read_json, require_execution_freeze, sha256
from time_reference import (
    array_hash,
    envelope_power,
    fine_runs,
    read_iq,
    wav_header,
)

ROOT = Path(__file__).resolve().parents[2]
PROTOCOL = (
    ROOT / "docs/implementation/wideband-detection/M4/validation-protocol.json"
)
BUDGET = 256 * 1024 * 1024
ATOL, RTOL = 1e-7, 1e-3


def write_json(path: Path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
        encoding="utf-8",
    )


def exact_sample(value, name):
    if (
        not isinstance(value, str)
        or not value
        or any(c not in "0123456789" for c in value)
        or int(value) > 2**64 - 1
    ):
        raise ValueError(f"{name} must be a uint64 decimal string")
    return int(value)


def config_values(config):
    if config.get("schema") != "signalstudio-sample-energy-parity-config-v1":
        raise ValueError("Unknown parity config schema")
    a = exact_sample(config["startSample"], "startSample")
    b = exact_sample(config["endSample"], "endSample")
    numeric = {
        k: config[k]
        for k in ("sampleRateHz", "centerFrequencyHz", "smoothSeconds")
    }
    band = config["bandRFHz"]
    numeric.update(lowerHz=band["lowerHz"], upperHz=band["upperHz"])
    numeric.update(
        sampleEdgeRatio=config.get("sampleEdgeRatio", 0.6),
        minOnSeconds=config.get("minOnSeconds", 0.005),
        minOffSeconds=config.get("minOffSeconds", 0.01),
        windowCoverage=config.get("windowCoverage", 0.0),
    )
    if any(
        isinstance(v, bool)
        or not isinstance(v, (int, float))
        or not math.isfinite(v)
        for v in numeric.values()
    ):
        raise ValueError("Config requires finite JSON numbers")
    fs, center = numeric["sampleRateHz"], numeric["centerFrequencyHz"]
    lo, hi = numeric["lowerHz"], numeric["upperHz"]
    split = config.get("splitBursts", True)
    semantics = config.get("referenceSemantics", "gui-frozen")
    if (
        not 0 <= a < b
        or fs <= 0
        or not center - fs / 2 <= lo < hi <= center + fs / 2
        or any(
            not 0 <= numeric[k] <= 60
            for k in ("smoothSeconds", "minOnSeconds", "minOffSeconds")
        )
        or not 0 <= numeric["sampleEdgeRatio"] <= 1
        or not 0 <= numeric["windowCoverage"] <= 1
        or not isinstance(split, bool)
        or semantics not in ("gui-frozen", "product-v2")
    ):
        raise ValueError("Invalid source, band or refinement config")
    return (
        a,
        b,
        {**numeric, "splitBursts": split, "referenceSemantics": semantics},
    )


def frozen_paths(frozen):
    entries = frozen.get("files", [])
    if isinstance(entries, dict):
        return {Path(k).resolve() for k in entries}
    return {Path(item["path"]).resolve() for item in entries}


def require_bound(frozen, paths):
    present = frozen_paths(frozen)
    for path in paths:
        if path.resolve() not in present:
            raise ValueError(
                f"Identity must be included in execution freeze: {path}"
            )


def load_native_array(item, count):
    path = Path(item["path"])
    if (
        item["dtype"] != "float32-little-endian"
        or exact_sample(item["samples"], "artifact samples") != count
        or path.stat().st_size != count * 4
        or sha256(path) != item["sha256"]
    ):
        raise ValueError(f"Native array identity/shape mismatch: {path}")
    values = np.fromfile(path, dtype="<f4")
    if not np.isfinite(values).all() or (values < 0).any():
        raise ValueError(f"Nonfinite/negative native energy: {path}")
    return values


def rolling_f64(power, win):
    # Same order as the native loop: add current, subtract expired, divide,
    # then store float32. A float64 prefix is not substituted for this loop.
    result = np.empty(len(power), dtype=np.float32)
    running = 0.0
    for i, value in enumerate(power):
        running += float(value)
        if i >= win:
            running -= float(power[i - win])
        result[i] = running / win
    return result


def cumulative_f32(power, win):
    pad = F.pad(power, (win - 1, 0))
    cumulative = F.pad(torch.cumsum(pad, dim=-1), (1, 0))
    return ((cumulative[win:] - cumulative[:-win]) / win).cpu().numpy()


def difference_report(actual, reference, output, name):
    difference = actual.astype(np.float64) - reference.astype(np.float64)
    absolute = np.abs(difference)
    limits = ATOL + RTOL * np.abs(reference.astype(np.float64))
    indices = np.flatnonzero(absolute > limits)
    difference_path = output / f"{name}.signed-diff.f64"
    index_path = output / f"{name}.violations.u64"
    difference.astype("<f8").tofile(difference_path)
    indices.astype("<u8").tofile(index_path)
    return {
        "samples": str(len(actual)),
        "atol": ATOL,
        "rtol": RTOL,
        "maxAbs": float(absolute.max()),
        "maxAbsLocalSample": str(int(absolute.argmax())),
        "meanAbs": float(absolute.mean()),
        "p50Abs": float(np.percentile(absolute, 50)),
        "p95Abs": float(np.percentile(absolute, 95)),
        "p99Abs": float(np.percentile(absolute, 99)),
        "maxToleranceRatio": float(np.max(absolute / limits)),
        "overToleranceCount": int(len(indices)),
        "firstViolationLocalSamples": [str(int(i)) for i in indices[:32]],
        "signedDifference": {
            "path": str(difference_path),
            "dtype": "float64-little-endian",
            "sha256": sha256(difference_path),
        },
        "violationIndices": {
            "path": str(index_path),
            "dtype": "uint64-little-endian",
            "sha256": sha256(index_path),
        },
    }


def decision(smoothed, settings, origin, *, native_policy):
    noise = float(np.percentile(smoothed, 0.5))
    burst = float(np.percentile(smoothed, 50))
    continuous = settings["windowCoverage"] >= 0.7
    product = native_policy or settings["referenceSemantics"] == "product-v2"
    fraction = settings["sampleEdgeRatio"]
    if continuous:
        fraction = fraction * (0.5 / 0.6) if product else 0.5
    threshold = noise + fraction * (burst - noise)
    comparison = float(np.float32(threshold))
    mask = smoothed > np.float32(threshold)
    min_on = max(
        2, int(max(0.0005, settings["minOnSeconds"]) * settings["sampleRateHz"])
    )
    min_off = max(2, int(settings["minOffSeconds"] * settings["sampleRateHz"]))
    if product and not settings["splitBursts"]:
        min_on = max(2, int(0.0005 * settings["sampleRateHz"]))
        min_off = 0 if native_policy else 2
    split = settings["splitBursts"] and not continuous
    raw_runs = fine_runs(mask, min_on, min_off, split)
    reason = ""
    if burst <= 1.3 * noise:
        reason = "sample-burst-noise-ratio-too-low"
    elif mask.mean() < 0.03:
        reason = "sample-on-fraction-too-low"
    elif not raw_runs:
        reason = "sample-no-sustained-on-run"
    runs = [] if reason else raw_runs
    return {
        "noise": noise,
        "burst": burst,
        "threshold": threshold,
        "comparisonThreshold": comparison,
        "effectiveEdgeRatio": fraction,
        "onFraction": float(mask.mean()),
        "minOnSamples": str(min_on),
        "minOffSamples": str(min_off),
        "continuousTrack": continuous,
        "splittingApplied": split,
        "status": "fallback" if reason else "applied",
        "fallbackReason": reason,
        "diagnosticMaskAvailableBeforeFallback": True,
        "maskSha256": array_hash(np.packbits(mask)),
        "rawIntegerRuns": [[str(a), str(b)] for a, b in raw_runs],
        "segments": [
            {"startSample": str(origin + a), "endSample": str(origin + b)}
            for a, b in runs
        ],
    }, mask


def scalar_compare(native, reference):
    error = abs(native - reference)
    limit = ATOL + RTOL * abs(reference)
    return {
        "native": native,
        "reference": reference,
        "absError": error,
        "limit": limit,
        "withinTolerance": error <= limit,
    }


def edge_diagnostics(native_segments, reference_segments, fs):
    same_count = len(native_segments) == len(reference_segments)
    pairs = []
    if same_count:
        for native, reference in zip(
            native_segments, reference_segments, strict=True
        ):
            a = int(native["startSample"]) - int(reference["startSample"])
            b = int(native["endSample"]) - int(reference["endSample"])
            pairs.append(
                {
                    "startSignedErrorSamples": str(a),
                    "endSignedErrorSamples": str(b),
                    "startSignedErrorSeconds": a / fs,
                    "endSignedErrorSeconds": b / fs,
                }
            )
    return {
        "nativeCount": len(native_segments),
        "referenceCount": len(reference_segments),
        "equalCount": same_count,
        "exactIntegerEndpoints": native_segments == reference_segments,
        "orderedPairsOnlyIfEqualCount": pairs,
    }


def run(args):
    started = time.monotonic()
    frozen = require_execution_freeze(args.freeze, Path(__file__))
    config = read_json(args.config)
    a, b, settings = config_values(config)
    native = read_json(args.native / "native-energy.json")
    if native["schema"] != "signalstudio-native-sample-energy-parity-v1":
        raise ValueError("Unknown native export schema")
    require_bound(
        frozen,
        [
            args.file,
            args.config,
            Path(native["executablePath"]),
            Path(__file__).with_name("time_reference.py"),
            PROTOCOL,
        ],
    )
    protocol = read_json(PROTOCOL)
    tolerance = protocol["tolerances"]["samplePowerEnvelopeThreshold"]
    if tolerance != {"atol": ATOL, "rtol": RTOL}:
        raise ValueError(
            "Tool constants differ from the pre-result frozen protocol"
        )
    fs, physical = wav_header(args.file)
    if (
        settings["sampleRateHz"] != fs
        or b > physical
        or native["config"] != config
        or native["configSha256"] != sha256(args.config)
        or native["sourceSha256"] != sha256(args.file)
        or native["executableSha256"] != sha256(Path(native["executablePath"]))
        or int(native["sourceStartSample"]) != a
        or int(native["sourceEndSample"]) != b
        or int(native["fftSamples"]) != b - a
        or int(native["workingByteBudget"]) != BUDGET
    ):
        raise ValueError(
            "Native and independent source/config/executable mapping differs"
        )
    count = b - a
    win = max(1, int(settings["smoothSeconds"] * fs))
    # Conservative live array/scratch allowance, separate from native accounting.
    # Both budgets exclude imported runtime/library baseline and native file mmap.
    if count * 128 + win * 16 + 2 * 1024 * 1024 > BUDGET:
        raise ValueError(
            "Parity exact ROI/smoothing exceeds fixed 256 MiB working budget"
        )
    if args.device not in ("cpu", "cuda:0"):
        raise ValueError("Use cpu or cuda:0")
    if args.device == "cuda:0" and not torch.cuda.is_available():
        raise ValueError("cuda:0 unavailable; no implicit CPU substitution")
    args.output.mkdir(parents=True, exist_ok=True)
    power_native = load_native_array(native["power"], count)
    smooth_native = load_native_array(native["smoothed"], count)
    iq = read_iq(args.file, a, b)
    relative = (settings["lowerHz"] + settings["upperHz"]) / 2 - settings[
        "centerFrequencyHz"
    ]
    bandwidth = settings["upperHz"] - settings["lowerHz"]
    if args.device == "cuda:0":
        torch.cuda.reset_peak_memory_stats()
    with torch.no_grad():
        reference_tensor = envelope_power(
            iq, fs, relative, bandwidth, args.device
        )
        power_reference = reference_tensor.cpu().numpy().copy()
        smooth_reference = cumulative_f32(reference_tensor, win)
        del reference_tensor, iq
        same_power = cumulative_f32(
            torch.from_numpy(power_native).to(args.device), win
        )
    if (
        not np.isfinite(power_reference).all()
        or not np.isfinite(smooth_reference).all()
    ):
        raise ValueError("Nonfinite independent energy/smoothing")
    reports = {}
    for name, actual, reference in (
        ("power", power_native, power_reference),
        ("smoothed", smooth_native, smooth_reference),
        ("same-native-power-smoothing", smooth_native, same_power),
    ):
        reports[name] = difference_report(actual, reference, args.output, name)
    del same_power
    mirror = rolling_f64(power_native, win)
    mirror_exact = np.array_equal(mirror, smooth_native)
    del mirror
    reference_decision, reference_mask = decision(
        smooth_reference, settings, a, native_policy=False
    )
    native_decision, native_mask = decision(
        smooth_native, settings, a, native_policy=True
    )
    observed = native["nativeRefinement"]
    # Full native rolling storage is exported by a mirror loop; verify its
    # percentiles and decisions against the production domain result as well.
    observed_checks = {
        k: observed.get(k) == native_decision[k]
        for k in (
            "noise",
            "burst",
            "status",
            "fallbackReason",
            "segments",
            "minOnSamples",
            "minOffSamples",
        )
    }
    if observed.get("threshold") is not None:
        observed_checks.update(
            {
                k: observed[k] == native_decision[k]
                for k in ("threshold", "comparisonThreshold", "onFraction")
            }
        )
    mismatches = np.flatnonzero(native_mask != reference_mask)
    ref_threshold = reference_decision["comparisonThreshold"]
    native_fixed_mask = smooth_native > np.float32(ref_threshold)
    ambiguity = (ATOL + RTOL * np.abs(smooth_reference.astype(np.float64))) + (
        ATOL + RTOL * abs(ref_threshold)
    )
    critical = (
        np.abs(smooth_reference.astype(np.float64) - ref_threshold) <= ambiguity
    )
    noncritical_mismatches = int(
        np.count_nonzero((native_mask != reference_mask) & ~critical)
    )
    mask_path = args.output / "mask-mismatch-local-samples.u64"
    mismatches.astype("<u8").tofile(mask_path)
    for label, values in (
        ("reference-power", power_reference),
        ("reference-smoothed", smooth_reference),
    ):
        values.astype("<f4").tofile(args.output / f"{label}.f32")
    reference_artifacts = {}
    for label in ("reference-power", "reference-smoothed"):
        path = args.output / f"{label}.f32"
        reference_artifacts[label] = {
            "path": str(path),
            "dtype": "float32-little-endian",
            "samples": str(count),
            "sha256": sha256(path),
        }
    for label, values in (
        ("native-mask", native_mask),
        ("reference-mask", reference_mask),
    ):
        path = args.output / f"{label}.u8"
        values.astype(np.uint8).tofile(path)
        reference_artifacts[label] = {
            "path": str(path),
            "dtype": "uint8-one-value-per-source-sample",
            "samples": str(count),
            "sha256": sha256(path),
            "diagnosticOnlyAfterProductionFallback": observed["status"]
            == "fallback",
        }
    edges = edge_diagnostics(
        observed["segments"], reference_decision["segments"], fs
    )
    scalars = {
        key: scalar_compare(native_decision[key], reference_decision[key])
        for key in ("noise", "burst", "threshold", "comparisonThreshold")
    }
    masks_exact = not len(mismatches)
    status_exact = (observed["status"], observed["fallbackReason"]) == (
        reference_decision["status"],
        reference_decision["fallbackReason"],
    )
    array_pass = all(
        item["overToleranceCount"] == 0
        for key, item in reports.items()
        if key in ("power", "smoothed")
    )
    pass_result = (
        array_pass
        and all(item["withinTolerance"] for item in scalars.values())
        and mirror_exact
        and all(observed_checks.values())
        and masks_exact
        and edges["exactIntegerEndpoints"]
        and status_exact
    )
    report = {
        "schema": "signalstudio-same-source-energy-parity-v1",
        "pass": pass_result,
        "scope": "one exact physical source ROI and declared RF band; not full M4 model/effect gate",
        "sourceKind": config.get(
            "sourceKind", "unspecified-do-not-infer-real-RF"
        ),
        "freezePath": str(args.freeze.resolve()),
        "freezeSha256": sha256(args.freeze),
        "config": config,
        "configSha256": sha256(args.config),
        "nativeReport": str((args.native / "native-energy.json").resolve()),
        "nativeReportSha256": sha256(args.native / "native-energy.json"),
        "protocolSha256": sha256(PROTOCOL),
        "sourceSha256": native["sourceSha256"],
        "device": args.device,
        "torchVersion": torch.__version__,
        "numpyVersion": np.__version__,
        "workingByteBudget": str(BUDGET),
        "estimatedLiveWorkingBytes": str(
            count * 128 + win * 16 + 2 * 1024 * 1024
        ),
        "budgetDefinition": "conservative array/scratch estimate excluding process/runtime baseline; not an OS hard RSS limit",
        "cudaPeakAllocatedBytes": (
            str(torch.cuda.max_memory_allocated())
            if args.device == "cuda:0"
            else None
        ),
        "elapsedSeconds": time.monotonic() - started,
        "support": {
            "startSample": str(a),
            "endSample": str(b),
            "fftSamples": str(count),
            "sourceStepSamples": "1",
            "sampleRateHz": fs,
            "relativeCenterHz": relative,
            "bandwidthHz": bandwidth,
            "smoothingSamples": str(win),
            "leftZeroPaddingSamples": str(win - 1),
        },
        "numericalVersions": {
            "energy": native["nativeRefinement"]["energyMethod"],
            "nativeSmoothing": "f64-causal-rolling-f32-storage",
            "referenceSmoothing": "frozen-torch-float32-cumsum",
            "referenceSemantics": settings["referenceSemantics"],
        },
        "arrays": reports,
        "referenceAndMaskArtifacts": reference_artifacts,
        "nativeRollingMirrorExact": mirror_exact,
        "nativeProductionDecisionMirrorChecks": observed_checks,
        "scalarDiagnostics": scalars,
        "nativeProduction": observed,
        "nativeFromFullSmoothedDiagnostic": native_decision,
        "reference": reference_decision,
        "mask": {
            "exact": masks_exact,
            "differentSamplesOwnThresholds": int(len(mismatches)),
            "differentSamplesReferenceThreshold": int(
                np.count_nonzero(native_fixed_mask != reference_mask)
            ),
            "nearThresholdReferenceSamples": int(critical.sum()),
            "noncriticalDifferentSamples": noncritical_mismatches,
            "criticalDefinition": "distance to reference comparison threshold <= pre-frozen value tolerance plus pre-frozen threshold tolerance; diagnostic only",
            "firstMismatchSourceSamples": [
                str(a + int(i)) for i in mismatches[:32]
            ],
            "allMismatchLocalIndicesPath": str(mask_path),
            "allMismatchLocalIndicesSha256": sha256(mask_path),
        },
        "edges": edges,
        "statusAndReasonExact": status_exact,
        "equivalenceClaim": (
            "all scoped checks passed"
            if pass_result
            else "not equivalent under frozen scoped checks"
        ),
    }
    write_json(args.output / "energy-parity.json", report)
    return report


def self_check():
    assert exact_sample("18446744073709551615", "sample") == 2**64 - 1
    for invalid in (123, "-1", "1.0", "18446744073709551616", ""):
        try:
            exact_sample(invalid, "sample")
        except ValueError:
            pass
        else:
            raise AssertionError("Invalid sample accepted")
    for n in (15, 17, 1000):
        iq = np.ones(n, dtype=np.complex64)
        values = envelope_power(iq, 1000, 0, 80, "cpu").numpy()
        assert len(values) == n and np.max(np.abs(values - 1)) < 1e-4
    power = np.asarray([1, 2, 4, 8], dtype=np.float32)
    assert np.array_equal(
        rolling_f64(power, 3),
        np.asarray([1 / 3, 1, 7 / 3, 14 / 3], dtype=np.float32),
    )
    assert np.array_equal(
        cumulative_f32(torch.from_numpy(power), 3), rolling_f64(power, 3)
    )
    assert fine_runs(np.asarray([0, 1, 1, 0, 1, 1, 1, 0]), 3, 2) == [(4, 7)]
    assert scalar_compare(ATOL, 0)["withinTolerance"]
    assert not scalar_compare(2 * ATOL, 0)["withinTolerance"]
    return {
        "passed": True,
        "device": "cpu",
        "checks": [
            "exact-uint64-parser",
            "arbitrary-FFT-length-reference",
            "causal-left-zero-padding",
            "float64-rolling-order",
            "drop-short-before-merge",
            "pre-frozen-tolerance-boundary",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--file", type=Path)
    parser.add_argument("--config", type=Path)
    parser.add_argument("--native", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--freeze", type=Path)
    parser.add_argument("--device", default="cpu", choices=("cpu", "cuda:0"))
    parser.add_argument("--self-check", action="store_true")
    args = parser.parse_args()
    if args.self_check:
        print(json.dumps(self_check(), indent=2))
        return 0
    if not all((args.file, args.config, args.native, args.output, args.freeze)):
        parser.error(
            "--file, --config, --native, --output and --freeze are required"
        )
    report = run(args)
    print(
        json.dumps(
            {
                "pass": report["pass"],
                "report": str(args.output / "energy-parity.json"),
                "powerViolations": report["arrays"]["power"][
                    "overToleranceCount"
                ],
                "smoothedViolations": report["arrays"]["smoothed"][
                    "overToleranceCount"
                ],
                "maskDifferences": report["mask"][
                    "differentSamplesOwnThresholds"
                ],
                "exactIntegerEndpoints": report["edges"][
                    "exactIntegerEndpoints"
                ],
            },
            indent=2,
        )
    )
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
