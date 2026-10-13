"""Compare every persisted M3 window and final frequency track independently.

Only actual WAV samples and stored raw model heads are inputs. External GUI/SCN
business modules are never imported or executed. This oracle verifies frequency
math, not a time-event detector or the historical two-dimensional F1 result.
"""

from __future__ import annotations

import argparse
from copy import deepcopy
from dataclasses import dataclass, field
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import sys
import wave

import numpy as np
import torch

sys.dont_write_bytecode = True

BIN_TOLERANCE = 0.05
SCORE_TOLERANCE = 0.001
PSD_TOLERANCE_DB = 0.02
MAX_WINDOWS = 512
MAX_SPECTRAL_VALUES = 64 * 1024 * 1024


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def read_json(path: Path):
    if path.stat().st_size > 16 * 1024 * 1024:
        raise ValueError(f"JSON exceeds the oracle input budget: {path}")

    def reject_nonfinite(token):
        raise ValueError(f"Nonfinite JSON constant: {token}")

    return json.loads(
        path.read_text(encoding="utf-8-sig"), parse_constant=reject_nonfinite
    )


def integer(value) -> int:
    if not isinstance(value, str) or not value.isascii() or not value.isdigit():
        raise ValueError(
            f"Expected canonical uint64 string, received {value!r}"
        )
    number = int(value)
    if str(number) != value or number >= 2**64:
        raise ValueError("Noncanonical or overflowing integer")
    return number


def bounded_path(root: Path, relative: str) -> Path:
    result = (root / relative).resolve(strict=True)
    if not result.is_relative_to(root.resolve()) or not result.is_file():
        raise ValueError("Artifact reference escapes its profile directory")
    return result


def asset_objects(value):
    if isinstance(value, dict):
        if "path" in value and "sha256" in value:
            yield value
        for child in value.values():
            yield from asset_objects(child)
    elif isinstance(value, list):
        for child in value:
            yield from asset_objects(child)


def audit_sources(assets) -> list[dict]:
    """Check mathematical reference identities; never load their Python code."""
    wanted = (
        "FFSCN_TEST_GUI/business/tracker.py",
        "FFSCN_TEST_GUI/business/detection.py",
        "FFSCN_TEST_GUI/algorithm/nms.py",
        "algorithm/refine/BandwidthRefiner.cpp",
    )
    result = []
    entries = list(asset_objects(assets))
    for suffix in wanted:
        matches = [
            entry
            for entry in entries
            if entry["path"].replace("\\", "/").endswith(suffix)
        ]
        if not matches:
            raise ValueError(
                f"M0 did not pin required math reference: {suffix}"
            )
        entry = matches[0]
        actual = sha256(Path(entry["path"]))
        if actual != entry["sha256"]:
            raise ValueError(f"M0 mathematical source changed: {entry['path']}")
        result.append(
            {
                "path": entry["path"],
                "sha256": actual,
                "m0Pinned": True,
                "imported": False,
            }
        )
    for relative in (
        "algorithm/refine/CnrRefiner.cpp",
        "algorithm/fusion/SignalFusion.cpp",
    ):
        path = Path("D:/project/SCN_based_PSD_Detection") / relative
        result.append(
            {
                "path": str(path),
                "sha256": sha256(path),
                "m0Pinned": False,
                "imported": False,
            }
        )
    return result


def audit_frozen_models(assets) -> list[dict]:
    entries = list(asset_objects(assets))
    result = []
    for suffix in (
        "FFSCN_TEST_GUI/config/epoch_1200.pth",
        "models/scn_model.engine",
    ):
        matches = [
            entry
            for entry in entries
            if entry["path"].replace("\\", "/").endswith(suffix)
        ]
        if not matches:
            raise ValueError(f"M0 model asset identity is missing: {suffix}")
        item = matches[0]
        actual = sha256(Path(item["path"]))
        if actual != item["sha256"]:
            raise ValueError(f"Frozen model asset changed: {item['path']}")
        result.append({"path": item["path"], "sha256": actual})
    return result


def overlap(a, b) -> float:
    intersection = max(0.0, min(a[1], b[1]) - max(a[0], b[0]))
    denominator = max(1e-9, max(a[1], b[1]) - min(a[0], b[0]))
    return intersection / denominator


def decode_heads(
    heads: np.ndarray,
    parameters,
    input_bins: int,
    scn: bool,
    float_geometry: bool = True,
) -> list[dict]:
    """Rank local maxima, then perform kept-candidate-only interval NMS."""
    if heads.shape != (3, input_bins // 4) or not np.isfinite(heads).all():
        raise ValueError("Raw heads have an invalid shape or nonfinite value")
    heat, width, offset = heads
    if (
        np.any(heat < 0)
        or np.any(heat > 1)
        or np.any(offset < 0)
        or np.any(offset > 1)
        or np.any(width < 0)
    ):
        raise ValueError("Heads must already contain valid activated values")
    radius = 1 if scn else 3
    cutoff = np.float32(parameters["scoreThreshold"])
    indices = []
    for index, score in enumerate(heat):
        threshold_pass = score >= cutoff if scn else score > cutoff
        local = np.max(heat[max(0, index - radius) : index + radius + 1])
        peak = abs(score - local) < np.float32(1e-6) if scn else score == local
        if threshold_pass and peak:
            indices.append(index)
    indices.sort(key=lambda index: (-float(heat[index]), index))
    candidates = []
    for index in indices[: int(parameters["topK"])]:
        if scn or not float_geometry:
            center = (index + float(offset[index])) * 4.0
            diameter = float(width[index]) * 4.0
            lower, upper = center - diameter / 2, center + diameter / 2
        else:
            center = np.float32(np.float32(index) + offset[index]) * np.float32(
                4
            )
            diameter = width[index] * np.float32(4)
            lower = float(np.float32(center - diameter * np.float32(0.5)))
            upper = float(np.float32(center + diameter * np.float32(0.5)))
        lower = max(0.0, min(float(input_bins - 1), lower))
        upper = max(0.0, min(float(input_bins - 1), upper))
        if upper <= lower:
            continue
        suppressed = False
        for previous in candidates:
            intersection = max(
                0.0,
                min(upper, previous["upperBin"])
                - max(lower, previous["lowerBin"]),
            )
            union = (
                upper
                - lower
                + previous["upperBin"]
                - previous["lowerBin"]
                - intersection
            )
            iou = intersection / union
            if (
                iou >= parameters["nmsIou"]
                if scn
                else iou > parameters["nmsIou"]
            ):
                suppressed = True
                break
        if not suppressed:
            candidates.append(
                {
                    "lowerBin": lower,
                    "upperBin": upper,
                    "score": float(heat[index]),
                    "peakIndex": str(index),
                }
            )
    return candidates


def compare_candidates(expected, actual, stage: str) -> dict:
    count_match = len(expected) == len(actual)
    order_match = count_match and all(
        a["peakIndex"] == b["peakIndex"] for a, b in zip(expected, actual)
    )
    edge_error = score_error = 0.0
    if count_match:
        for a, b in zip(expected, actual):
            edge_error = max(
                edge_error,
                abs(a["lowerBin"] - b["lowerBin"]),
                abs(a["upperBin"] - b["upperBin"]),
            )
            score_error = max(score_error, abs(a["score"] - b["score"]))
    return {
        "stage": stage,
        "expectedCount": len(expected),
        "actualCount": len(actual),
        "countsMatch": count_match,
        "peakOrderMatch": order_match,
        "maxEdgeBins": edge_error if count_match else None,
        "maxScore": score_error if count_match else None,
        "passed": bool(
            order_match
            and edge_error <= BIN_TOLERANCE
            and score_error <= SCORE_TOLERANCE
        ),
    }


@dataclass
class Track:
    low: float
    high: float
    nominal: tuple[int, int]
    actual: tuple[int, int]
    seed: tuple[int, int, str]
    windows: dict[str, dict] = field(default_factory=dict)
    misses: int = 0
    legacy_sum: float = 0.0
    legacy_weight: int = 0

    def summary(self) -> dict:
        scores = [window["score"] for window in self.windows.values()]
        lower = [
            sum(band["lowerHz"] for band in window["bands"])
            / len(window["bands"])
            for window in self.windows.values()
        ]
        upper = [
            sum(band["upperHz"] for band in window["bands"])
            / len(window["bands"])
            for window in self.windows.values()
        ]
        return {
            "scoreMean": math.fsum(scores) / len(scores),
            "scoreMax": max(scores),
            "originalLowerHz": math.fsum(lower) / len(lower),
            "originalUpperHz": math.fsum(upper) / len(upper),
            "legacyDuplicateWeightedScoreMean": self.legacy_sum
            / self.legacy_weight,
        }


def evidence(mapping, candidate, ordinal: int) -> dict:
    first, step = mapping["frequencyFirstBinHz"], mapping["frequencyStepHz"]
    band = {
        **candidate,
        "lowerHz": first + candidate["lowerBin"] * step,
        "upperHz": first + candidate["upperBin"] * step,
    }
    return {
        "windowId": mapping["windowId"],
        "ordinal": str(ordinal),
        "score": candidate["score"],
        "bands": [band],
        "nominalStartSample": mapping["nominalStartSample"],
        "nominalEndSample": mapping["nominalEndSample"],
        "contextStartSample": mapping["contextStartSample"],
        "contextEndSample": mapping["contextEndSample"],
    }


def absorb(a: Track, b: Track) -> Track:
    merged = deepcopy(a)
    merged.low, merged.high = min(a.low, b.low), max(a.high, b.high)
    merged.nominal = min(a.nominal[0], b.nominal[0]), max(
        a.nominal[1], b.nominal[1]
    )
    merged.actual = min(a.actual[0], b.actual[0]), max(a.actual[1], b.actual[1])
    merged.seed = min(a.seed, b.seed)
    merged.legacy_sum += b.legacy_sum
    merged.legacy_weight += b.legacy_weight
    for key, item in b.windows.items():
        if key not in merged.windows:
            merged.windows[key] = deepcopy(item)
        else:
            merged.windows[key]["score"] = max(
                merged.windows[key]["score"], item["score"]
            )
            merged.windows[key]["bands"].extend(deepcopy(item["bands"]))
            merged.windows[key]["bands"].sort(
                key=lambda band: int(band["peakIndex"])
            )
    return merged


def reconcile(
    tracks: list[Track],
    merge_hz: float,
    match_iou: float,
    center_slack: float,
    gap_samples: int,
    fragments: bool,
) -> list[Track]:
    """Find one global best pair per iteration, preserving original evidence."""
    result = deepcopy(tracks)
    while True:
        choices = []
        for i, a in enumerate(result):
            for j in range(i + 1, len(result)):
                b = result[j]
                time_gap = max(a.nominal[0], b.nominal[0]) - min(
                    a.nominal[1], b.nominal[1]
                )
                if not fragments:
                    gap_hz = max(a.low, b.low) - min(a.high, b.high)
                    if merge_hz > 0 and gap_hz <= merge_hz and time_gap < 0:
                        choices.append(((gap_hz, time_gap, i, j), i, j))
                    continue
                intersection = min(a.high, b.high) - max(a.low, b.low)
                if time_gap > gap_samples or intersection <= 0:
                    continue
                iou = overlap((a.low, a.high), (b.low, b.high))
                containment = intersection / max(
                    1e-9, min(a.high - a.low, b.high - b.low)
                )
                distance = abs((a.low + a.high - b.low - b.high) / 2)
                center_limit = (
                    min(a.high - a.low, b.high - b.low) / 2 + center_slack
                )
                if iou >= match_iou or (
                    containment >= match_iou and distance <= center_limit
                ):
                    choices.append(
                        (
                            (
                                max(0, time_gap),
                                -max(iou, containment),
                                distance,
                                min(a.nominal[0], b.nominal[0]),
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
        result[i] = absorb(result[i], result[j])
        result.pop(j)
    return result


def associate(windows, request, denominator: int) -> list[Track]:
    """Independent best-IoU one-to-one online tracker and frequency fusion."""
    parameters = request["parameters"]
    active, closed = [], []
    seen = set()
    for ordinal, (mapping, candidates) in enumerate(windows):
        key = mapping["windowId"]
        if key in seen:
            raise ValueError(
                "Duplicate source window ID would inflate unique hits"
            )
        seen.add(key)
        observations = [
            evidence(mapping, candidate, ordinal) for candidate in candidates
        ]
        edges = [
            (item["bands"][0]["lowerHz"], item["bands"][0]["upperHz"])
            for item in observations
        ]
        pairs = []
        for ti, track in enumerate(active):
            for di, band in enumerate(edges):
                iou = overlap((track.low, track.high), band)
                if iou >= 0.3:
                    pairs.append((-iou, -observations[di]["score"], ti, di))
        used_tracks, used_bands = set(), set()
        nominal = integer(mapping["nominalStartSample"]), integer(
            mapping["nominalEndSample"]
        )
        actual = integer(mapping["contextStartSample"]), integer(
            mapping["contextEndSample"]
        )
        for _, _, ti, di in sorted(pairs):
            if ti in used_tracks or di in used_bands:
                continue
            track = active[ti]
            track.windows[key] = observations[di]
            stats = track.summary()
            track.low, track.high = (
                stats["originalLowerHz"],
                stats["originalUpperHz"],
            )
            track.nominal = min(track.nominal[0], nominal[0]), max(
                track.nominal[1], nominal[1]
            )
            track.actual = min(track.actual[0], actual[0]), max(
                track.actual[1], actual[1]
            )
            track.legacy_sum += observations[di]["score"]
            track.legacy_weight += 1
            track.misses = 0
            used_tracks.add(ti)
            used_bands.add(di)
        surviving = []
        for ti, track in enumerate(active):
            if ti not in used_tracks:
                track.misses += 1
            (
                closed if track.misses > parameters["maxMiss"] else surviving
            ).append(track)
        active = surviving
        for di, item in enumerate(observations):
            if di in used_bands:
                continue
            low, high = edges[di]
            seed = ordinal, int(item["bands"][0]["peakIndex"]), key
            active.append(
                Track(
                    low,
                    high,
                    nominal,
                    actual,
                    seed,
                    {key: item},
                    legacy_sum=item["score"],
                    legacy_weight=1,
                )
            )
    minimum = min(parameters["minHits"], max(1, denominator))
    tracks = [
        track for track in closed + active if len(track.windows) >= minimum
    ]
    tracks = reconcile(tracks, parameters["mergeFrequencyHz"], 0.3, 0, 0, False)
    gap = 0.0
    if parameters["splitBursts"]:
        gap = (
            parameters["coarseSplitGapSeconds"]
            if parameters["timeStrategy"] == "coarse"
            else parameters["minOffSeconds"]
        )
    step = windows[0][0]["frequencyStepHz"]
    tracks = reconcile(
        tracks,
        0,
        0.3,
        2 * step,
        math.ceil(gap * request["sourceIdentity"]["sampleRateHz"]),
        True,
    )
    return sorted(
        tracks,
        key=lambda track: (track.low, track.nominal[0], track.high, track.seed),
    )


def gui_frequency(
    mean,
    floor_db: float,
    frequency,
    first_hz: float,
    step_hz: float,
    margin_db: float,
) -> dict:
    """Global-percentile gate, dominant contiguous width, half-bin boundaries."""
    if margin_db <= 0:
        return {
            "lowerHz": frequency[0],
            "upperHz": frequency[1],
            "skippedReason": "nonpositive-margin-disables-gui-refinement",
        }
    centres = first_hz + np.arange(mean.size) * step_hz
    a, b = np.clip(
        np.searchsorted(centres, frequency), 0, mean.size - 1
    ).tolist()
    a, b = int(a), int(b)
    peak = a + int(np.argmax(mean[a : b + 1]))
    threshold = floor_db + margin_db
    measured_a, measured_b = a, b
    corrected_a, corrected_b = a, b
    shrunk = False
    reason = ""
    if float(mean[peak]) <= threshold:
        reason = "peak-does-not-strictly-exceed-threshold"
    else:
        measured_a = measured_b = peak
        while measured_a > a and mean[measured_a - 1] > threshold:
            measured_a -= 1
        while measured_b < b and mean[measured_b + 1] > threshold:
            measured_b += 1
        measured_width = measured_b - measured_a + 1
        original_width = b - a + 1
        if measured_width < 0.5 * original_width:
            width = max(math.ceil(0.1 * original_width), measured_width)
            centre = (measured_a + measured_b) / 2
            corrected_a = max(a, math.floor(centre - (width - 1) / 2))
            corrected_b = min(b, corrected_a + width - 1)
            corrected_a = max(a, corrected_b - width + 1)
            shrunk = True
        else:
            reason = "measured-width-at-least-half-original"
    return {
        "lowerHz": centres[corrected_a] - 0.5 * step_hz,
        "upperHz": centres[corrected_b] + 0.5 * step_hz,
        "originalFirstBin": a,
        "originalLastBin": b,
        "measuredFirstBin": measured_a,
        "measuredLastBin": measured_b,
        "noiseFloorDb": floor_db,
        "thresholdDb": threshold,
        "peakDb": float(mean[peak]),
        "shrunk": shrunk,
        "skippedReason": reason,
    }


def scn_fuse(candidates) -> list[dict]:
    """Frequency-sorted interval union at the frozen IoU/containment gates."""
    ordered = sorted(
        deepcopy(candidates),
        key=lambda value: (
            value["lowerBin"],
            value["upperBin"],
            int(value["peakIndex"]),
        ),
    )
    result = []
    for item in ordered:
        if result:
            prior = result[-1]
            common = max(
                0.0,
                min(prior["upperBin"], item["upperBin"])
                - max(prior["lowerBin"], item["lowerBin"]),
            )
            widths = (
                prior["upperBin"] - prior["lowerBin"],
                item["upperBin"] - item["lowerBin"],
            )
            iou = common / (sum(widths) - common)
            contained = common / min(widths)
            if iou >= 0.1 or contained >= 0.5:
                prior["lowerBin"] = min(prior["lowerBin"], item["lowerBin"])
                prior["upperBin"] = max(prior["upperBin"], item["upperBin"])
                if item["score"] > prior["score"]:
                    prior["peakIndex"] = item["peakIndex"]
                prior["score"] = max(prior["score"], item["score"])
                continue
        result.append(item)
    return result


def scn_window(candidates, spectra, mapping) -> dict:
    """Maximum-spectrum CNR, fusion, then average-neighbor minimum edges."""
    maximum = np.max(spectra, axis=0)
    average = np.mean(spectra, axis=0, dtype=np.float64).astype(np.float32)
    count = maximum.size
    measurements, all_bands, accepted = [], [], []
    for raw in candidates:
        a, b = int(raw["lowerBin"]), int(raw["upperBin"])
        if b <= a:
            continue
        width = b - a
        trim = math.floor(width / 5 + 0.5)
        middle_a = min(b - 1, a + trim)
        middle_b = max(middle_a + 1, b - trim)
        signal = float(np.mean(maximum[middle_a:middle_b], dtype=np.float64))
        neighborhood = max(1, width // 10)
        neighbors = np.concatenate(
            (
                maximum[max(0, a - neighborhood) : a],
                maximum[b : min(count, b + neighborhood)],
            )
        )
        noise = (
            float(np.mean(neighbors, dtype=np.float64))
            if neighbors.size
            else signal
        )
        cnr = signal - noise
        measured = {
            **raw,
            "signalLevelDb": signal,
            "neighborLevelDb": noise,
            "cnrDb": cnr,
            "accepted": cnr >= 3,
        }
        measurements.append(measured)
        # Reference CnrRefiner publishes the integer measurement boundaries
        # to SignalFusion, while raw predictions remain in measurements.
        band = {**raw, "lowerBin": float(a), "upperBin": float(b)}
        all_bands.append(band)
        if measured["accepted"]:
            accepted.append(band)
    unfiltered, filtered = scn_fuse(all_bands), scn_fuse(accepted)
    occupied = np.zeros(count, dtype=np.bool_)
    for item in unfiltered:
        occupied[int(item["lowerBin"]) : int(item["upperBin"])] = True
    refinements, corrected = [], []
    first, step = mapping["frequencyFirstBinHz"], mapping["frequencyStepHz"]
    for item in filtered:
        a, b = int(item["lowerBin"]), int(item["upperBin"])
        original_low, original_high = first + a * step, first + b * step
        result = {
            "originalLowerHz": original_low,
            "originalUpperHz": original_high,
            "lowerHz": original_low,
            "upperHz": original_high,
            "applied": False,
            "skippedReason": "",
            "noiseSampleCount": "0",
        }
        neighborhood = max(1, (b - a) // 10)
        indices = np.concatenate(
            (
                np.arange(max(0, a - neighborhood), a),
                np.arange(b, min(count, b + neighborhood)),
            )
        )
        usable = indices[(~occupied[indices]) & np.isfinite(average[indices])]
        result["noiseSampleCount"] = str(usable.size)
        if not usable.size:
            result["skippedReason"] = "no-unoccupied-out-of-band-samples"
        else:
            floor = float(np.min(average[usable]))
            result["noiseFloorDb"], result["thresholdDb"] = floor, floor + 3
            on = np.flatnonzero(
                np.isfinite(maximum[a:b]) & (maximum[a:b] >= floor + 3)
            )
            if not on.size:
                result["skippedReason"] = "no-in-band-bin-meets-threshold"
            else:
                result["lowerHz"] = max(
                    original_low, first + (a + int(on[0])) * step
                )
                result["upperHz"] = min(
                    original_high, first + (a + int(on[-1]) + 1) * step
                )
                result["applied"] = (
                    result["lowerHz"] != original_low
                    or result["upperHz"] != original_high
                )
        refinements.append(result)
        corrected.append(
            {
                **item,
                "lowerBin": (result["lowerHz"] - first) / step,
                "upperBin": (result["upperHz"] - first) / step,
            }
        )
    corrected.sort(key=lambda item: -item["score"])
    return {
        "measurements": measurements,
        "unfilteredFused": unfiltered,
        "filteredFused": filtered,
        "refinements": refinements,
        "candidates": corrected[:150],
    }


def compare_scn_stages(expected, native, stored, step: float) -> dict:
    constants = {
        "version": "scn-max-cnr-fusion-min-bandwidth-before-tracking-v1",
        "cnrThresholdDb": 3,
        "bandwidthMarginDb": 3,
        "fusionIou": 0.1,
        "fusionOverlapRatio": 0.5,
        "fusionGapHz": 0,
        "candidateCap": 150,
        "meanSpectrumInferenceCalls": 0,
    }
    settings_match = all(
        native.get(key) == value for key, value in constants.items()
    )
    stages = [
        compare_candidates(expected[key], native[key], key)
        for key in ("unfilteredFused", "filteredFused")
    ]
    stages.append(
        compare_candidates(
            expected["candidates"], stored, "corrected-ranked-window-candidates"
        )
    )
    measurements = native["measurements"]
    measurement_pass = len(measurements) == len(expected["measurements"])
    max_cnr_error = 0.0
    if measurement_pass:
        for a, b in zip(expected["measurements"], measurements, strict=True):
            max_cnr_error = max(
                max_cnr_error,
                *(
                    abs(a[key] - b[key])
                    for key in ("signalLevelDb", "neighborLevelDb", "cnrDb")
                ),
            )
            measurement_pass &= (
                a["peakIndex"] == b["peakIndex"]
                and a["accepted"] == b["accepted"]
            )
    measurement_pass &= max_cnr_error <= PSD_TOLERANCE_DB
    refinements = native["refinements"]
    refinement_pass = len(refinements) == len(expected["refinements"])
    max_noise_error, max_edge_error = 0.0, 0.0
    if refinement_pass:
        for a, b in zip(expected["refinements"], refinements, strict=True):
            refinement_pass &= all(
                a[key] == b[key]
                for key in ("applied", "skippedReason", "noiseSampleCount")
            )
            for key in (
                "originalLowerHz",
                "originalUpperHz",
                "lowerHz",
                "upperHz",
            ):
                max_edge_error = max(max_edge_error, abs(a[key] - b[key]))
            refinement_pass &= ("noiseFloorDb" in a) == ("noiseFloorDb" in b)
            if "noiseFloorDb" in a and "noiseFloorDb" in b:
                max_noise_error = max(
                    max_noise_error,
                    abs(a["noiseFloorDb"] - b["noiseFloorDb"]),
                    abs(a["thresholdDb"] - b["thresholdDb"]),
                )
    refinement_pass &= (
        max_edge_error <= BIN_TOLERANCE * step
        and max_noise_error <= PSD_TOLERANCE_DB
    )
    return {
        "stage": "scn-window-cnr-fusion-minimum-refinement",
        "constantsMatch": settings_match,
        "candidateStages": stages,
        "measurementCount": len(expected["measurements"]),
        "acceptedMeasurements": sum(
            item["accepted"] for item in expected["measurements"]
        ),
        "cnrMaximumErrorDb": max_cnr_error,
        "measurementsMatch": bool(measurement_pass),
        "refinementMaximumEdgeErrorHz": max_edge_error,
        "refinementMaximumNoiseErrorDb": max_noise_error,
        "refinementsMatch": bool(refinement_pass),
        "passed": bool(
            settings_match
            and measurement_pass
            and refinement_pass
            and all(stage["passed"] for stage in stages)
        ),
    }


def load_windows(directory: Path, manifest):
    request = manifest["request"]
    windows = []
    publication, chunk_identities = [], []
    ended = False
    source_hash = None
    for sequence, ref in enumerate(manifest["chunks"]):
        if integer(ref["sequence"]) != sequence:
            raise ValueError("Chunk sequence is discontinuous")
        path = bounded_path(directory, ref["relativePath"])
        if sha256(path) != ref["sha256"]:
            raise ValueError("Canonical chunk hash mismatch")
        chunk = read_json(path)
        if (
            chunk["runId"] != request["runId"]
            or chunk["fileId"] != request["sourceIdentity"]["fileId"]
        ):
            raise ValueError("Chunk run/file/result mapping identity mismatch")
        if source_hash is None:
            source_hash = chunk["sourceSha256"]
        if chunk["sourceSha256"] != source_hash:
            raise ValueError("Source identity hash changed within the run")
        chunk_identities.append(
            {
                "sequence": sequence,
                "path": ref["relativePath"],
                "sha256": ref["sha256"],
            }
        )
        if not chunk["mappings"]:
            for record in chunk["results"]:
                if ended:
                    raise ValueError(
                        "Projection records follow a terminal publication marker"
                    )
                if record["kind"] == "frequency-publication-end-v1":
                    if integer(record["count"]) != len(publication):
                        raise ValueError(
                            "Durable publication count is inconsistent"
                        )
                    ended = True
                elif record["kind"] == "frequency-annotation-v1":
                    publication.append(record)
                else:
                    raise ValueError(
                        "Unsupported durable projection record kind"
                    )
            continue
        if (
            publication
            or ended
            or len(chunk["results"]) != len(chunk["mappings"])
        ):
            raise ValueError(
                "Raw windows must precede complete durable projections"
            )
        for mapping, result in zip(
            chunk["mappings"], chunk["results"], strict=True
        ):
            if mapping["windowId"] != result["windowId"]:
                raise ValueError("Result/window mapping identity mismatch")
            windows.append((mapping, result["candidates"]))
    if len(windows) > MAX_WINDOWS:
        raise ValueError("The bounded parity probe permits at most 512 windows")
    if not ended:
        raise ValueError(
            "Frequency parity requires a committed projection end marker"
        )
    return windows, publication, chunk_identities


def load_spectra(wav_path: Path, profile, windows, source_oracle):
    width = profile["run"]["request"]["profile"]["fftSize"]
    unique_frames = sorted(
        {
            integer(frame["frameIndex"])
            for mapping, _ in windows
            for frame in mapping["frames"]
        }
    )
    if not unique_frames or unique_frames != list(
        range(unique_frames[0], unique_frames[-1] + 1)
    ):
        raise ValueError(
            "Expected a nonempty contiguous unique source-frame context"
        )
    if len(unique_frames) * width > MAX_SPECTRAL_VALUES:
        raise ValueError(
            "Independent full context exceeds the bounded DSP oracle budget"
        )
    with wave.open(str(wav_path), "rb") as stream:
        if (
            stream.getnchannels(),
            stream.getsampwidth(),
            stream.getcomptype(),
        ) != (2, 2, "NONE"):
            raise ValueError("Frequency parity requires real PCM16 I/Q WAV")
        fs, count = stream.getframerate(), stream.getnframes()
    hop = math.floor(fs * 0.001)
    leading = integer(profile["sourceLeadingPadding"])
    expected_leading = max(0, width - hop - count % hop) // 2
    if leading != expected_leading:
        raise ValueError(
            "Source padding does not match the frozen full-file FFT grid"
        )
    for mapping, _ in windows:
        first = integer(mapping["firstFrameIndex"])
        if (
            integer(mapping["nominalStartSample"]) != first * hop
            or integer(mapping["nominalEndSample"])
            != (first + len(mapping["frames"])) * hop
        ):
            raise ValueError(
                "Nominal source window is not on the original fixed grid"
            )
        for frame in mapping["frames"]:
            index = integer(frame["frameIndex"])
            expected_start = max(0, index * hop - leading)
            expected_end = min(count, index * hop - leading + width)
            if (
                integer(frame["startSample"]) != expected_start
                or integer(frame["endSample"]) != expected_end
            ):
                raise ValueError(
                    "Actual frame support does not match source-padding semantics"
                )
    hann = torch.hann_window(
        width, periodic=True, dtype=torch.float32, device="cpu"
    )
    spectra = np.stack(
        [
            source_oracle.frame_density(
                wav_path, index, count, hop, width, leading, hann, fs
            ).numpy()
            for index in unique_frames
        ]
    )
    if not np.isfinite(spectra).all():
        raise ValueError(
            "Actual independent WAV spectra contain nonfinite values"
        )
    return unique_frames, spectra, fs, hop


def seed_id(track: Track, request) -> str:
    strings = (
        request["runId"],
        request["sourceIdentity"]["fileId"],
        track.seed[2],
    )
    return (
        "frequency-track:"
        + "".join(f"{len(value.encode('utf-8'))}:{value}" for value in strings)
        + f":{track.seed[1]}"
    )


def compare_tracks(
    tracks,
    annotations,
    request,
    frames,
    spectra,
    hop,
    denominator,
    scn: bool,
    window_mappings,
) -> dict:
    actual = {}
    for record in annotations:
        annotation = record["annotation"]
        origin = annotation["originEvidence"]
        fields = json.loads(origin["originalFieldsJson"])
        key = fields["seedId"]
        if key in actual:
            raise ValueError("Duplicate final track seed identity")
        if (
            annotation["segments"]
            or annotation.get("classLabel") is not None
            or annotation["observationTimeQuality"] != "WindowObservation"
            or annotation["sourceKind"] != "Algorithm"
            or annotation["fileId"] != request["sourceIdentity"]["fileId"]
        ):
            raise ValueError(
                "M3 frequency prediction must not invent time events or classes"
            )
        actual[key] = (annotation, origin, fields)
    first_hz = (
        request["sourceIdentity"]["centerFrequencyHz"]
        - request["sourceIdentity"]["sampleRateHz"] / 2
    )
    step = (
        request["sourceIdentity"]["sampleRateHz"]
        / request["profile"]["fftSize"]
    )
    floor_db = float(np.percentile(spectra, 10)) if not scn else None
    details = []
    for track in tracks:
        key = seed_id(track, request)
        item = {"seedId": key, "matched": key in actual}
        if key not in actual:
            item["passed"] = False
            details.append(item)
            continue
        annotation, origin, fields = actual[key]
        stats = track.summary()
        identifiers = sorted(
            track.windows,
            key=lambda value: int(track.windows[value]["ordinal"]),
        )
        hits = len(identifiers)
        structural = (
            integer(fields["uniqueHits"]) == hits
            and integer(fields["eligibleWindowDenominator"]) == denominator
            and sorted(origin["windowIds"], key=int)
            == sorted(identifiers, key=int)
            and integer(origin["windowCount"]) == denominator
        )
        expected_provenance = []
        for window_id in origin["windowIds"]:
            mapping = window_mappings[window_id]
            saved = {
                "windowId": window_id,
                "headEvidence": mapping["headEvidence"],
            }
            if scn:
                saved["scnPostprocess"] = mapping["scnPostprocess"]
            expected_provenance.append(saved)
        provenance_match = fields["windowPostprocess"] == expected_provenance
        expected_refs = [
            saved["headEvidence"]["path"] for saved in expected_provenance
        ]
        provenance_match &= origin["evidenceRefs"] == expected_refs
        structural &= provenance_match
        expected_annotation_id = (
            "detection:"
            + hashlib.sha256(
                (request["runId"] + ":" + key).encode("utf-8")
            ).hexdigest()
        )
        structural &= annotation["id"] == expected_annotation_id
        mean_error = abs(origin["scoreMean"] - stats["scoreMean"])
        max_error = abs(origin["scoreMax"] - stats["scoreMax"])
        coverage_error = abs(fields["windowCoverage"] - hits / denominator)
        original_error = max(
            abs(fields["originalLowerHz"] - stats["originalLowerHz"]),
            abs(fields["originalUpperHz"] - stats["originalUpperHz"]),
            abs(
                origin["originalObservation"]["lowerHz"]
                - stats["originalLowerHz"]
            ),
            abs(
                origin["originalObservation"]["upperHz"]
                - stats["originalUpperHz"]
            ),
        )
        if scn:
            expected_frequency = {"lowerHz": track.low, "upperHz": track.high}
            frequency_stage = (
                "per-window-scn-cnr-fusion-minimum-refinement-before-tracking"
            )
        else:
            first = max(
                frames[0], min(frames[-1], math.ceil(track.nominal[0] / hop))
            )
            end = max(
                frames[0] + 1,
                min(frames[-1] + 1, math.ceil(track.nominal[1] / hop)),
            )
            selected = spectra[first - frames[0] : end - frames[0]]
            # The GUI oracle's mean has float32 accumulation; native mean uses
            # float64 accumulation. Existing PSD tolerance is retained.
            mean = selected.mean(axis=0)
            expected_frequency = gui_frequency(
                mean,
                floor_db,
                (track.low, track.high),
                first_hz,
                step,
                request["parameters"]["bandwidthMarginDb"],
            )
            frequency_stage = "all-unique-frame-percentile-and-per-track-mean"
        observed_frequency = fields.get("frequencyAnalysis", {})
        observation = annotation["observation"]
        roi = request["requestedRange"]
        observed_low = max(
            roi["frequencyLowerHz"], expected_frequency["lowerHz"]
        )
        observed_high = min(
            roi["frequencyUpperHz"], expected_frequency["upperHz"]
        )
        corrected_error = max(
            abs(observation["lowerHz"] - observed_low),
            abs(observation["upperHz"] - observed_high),
        )
        structural &= integer(observation["start"]) == max(
            track.nominal[0], integer(roi["startSample"])
        ) and integer(observation["end"]) == min(
            track.nominal[1], integer(roi["endSample"])
        )
        noise_error = None
        noise_pass = True
        if not scn:
            if "noiseFloorDb" not in observed_frequency:
                noise_pass = False
            else:
                noise_error = abs(observed_frequency["noiseFloorDb"] - floor_db)
                noise_pass = noise_error <= PSD_TOLERANCE_DB
            structural &= (
                integer(observed_frequency["globalFrameCount"]) == len(frames)
                and integer(observed_frequency["percentileSampleCount"])
                == spectra.size
            )
        else:
            structural &= "frequencyAnalysis" not in fields
        window_rows = sorted(
            fields["windows"], key=lambda row: int(row["ordinal"])
        )
        expected_rows = sorted(
            track.windows.values(), key=lambda row: int(row["ordinal"])
        )
        evidence_pass = len(window_rows) == len(expected_rows)
        if evidence_pass:
            for expected, observed in zip(
                expected_rows, window_rows, strict=True
            ):
                evidence_pass &= all(
                    observed[name] == expected[name]
                    for name in (
                        "windowId",
                        "ordinal",
                        "nominalStartSample",
                        "nominalEndSample",
                        "contextStartSample",
                        "contextEndSample",
                    )
                )
                evidence_pass &= len(observed["bands"]) == len(
                    expected["bands"]
                )
                evidence_pass &= (
                    abs(observed["score"] - expected["score"])
                    <= SCORE_TOLERANCE
                )
                if len(observed["bands"]) == len(expected["bands"]):
                    for a, b in zip(
                        expected["bands"], observed["bands"], strict=True
                    ):
                        evidence_pass &= (
                            a["peakIndex"] == b["peakIndex"]
                            and abs(a["lowerHz"] - b["lowerHz"])
                            <= BIN_TOLERANCE * step
                            and abs(a["upperHz"] - b["upperHz"])
                            <= BIN_TOLERANCE * step
                            and abs(a["score"] - b["score"]) <= SCORE_TOLERANCE
                        )
        passed = (
            structural
            and evidence_pass
            and original_error <= BIN_TOLERANCE * step
            and corrected_error <= BIN_TOLERANCE * step
            and mean_error <= SCORE_TOLERANCE
            and max_error <= SCORE_TOLERANCE
            and coverage_error <= 1e-12
            and noise_pass
        )
        item.update(
            {
                "uniqueHits": hits,
                "eligibleDenominator": denominator,
                "statisticsAndIdentityMatch": structural,
                "immutableWindowProvenanceMatch": provenance_match,
                "windowEvidenceMatch": evidence_pass,
                "scoreMeanError": mean_error,
                "scoreMaxError": max_error,
                "coverageError": coverage_error,
                "legacyDuplicateWeightedScoreMean": stats[
                    "legacyDuplicateWeightedScoreMean"
                ],
                "uniqueWindowMaxScoreMean": stats["scoreMean"],
                "originalEdgeErrorHz": original_error,
                "correctedEdgeErrorHz": corrected_error,
                "frequencyStage": frequency_stage,
                "expectedFrequency": expected_frequency,
                "nativeFrequencyEvidence": observed_frequency,
                "noiseFloorErrorDb": noise_error,
                "passed": bool(passed),
            }
        )
        details.append(item)
    return {
        "expectedCount": len(tracks),
        "actualCount": len(actual),
        "globalUniqueFrames": len(frames),
        "globalFirstFrame": frames[0],
        "globalLastFrameExclusive": frames[-1] + 1,
        "globalPercentileSamples": int(spectra.size),
        "oracleNoiseFloorDb": floor_db,
        "tracks": details,
        "passed": bool(
            len(tracks) == len(actual)
            and all(item["passed"] for item in details)
        ),
    }


def verify_profile(pipeline_root, profile, wav_path, source_oracle) -> dict:
    name = profile["profileId"]
    directory = pipeline_root / name
    manifest_path = bounded_path(
        directory, profile["run"]["manifestRelativePath"]
    )
    manifest = read_json(manifest_path)
    request = manifest["request"]
    if request != profile["run"]["request"]:
        raise ValueError(
            "Canonical run differs from the selected live profile snapshot"
        )
    if (
        manifest["lifecycle"] != "Completed"
        or manifest["sourceValidity"] != "Current"
    ):
        raise ValueError(
            "Formal parity requires a completed run bound to its current source"
        )
    if request["profile"]["id"] != name:
        raise ValueError("Profile/run identity mismatch")
    windows, durable_annotations, chunk_identities = load_windows(
        directory, manifest
    )
    denominator = integer(manifest["eligibleWindows"])
    if (
        len(windows) != denominator
        or integer(manifest["completedWindows"]) != denominator
    ):
        raise ValueError(
            "All-window parity requires complete eligible coverage"
        )
    scn = name == "scn-max-psd-v1"
    width = int(request["profile"]["fftSize"])
    frames, spectra, fs, hop = load_spectra(
        wav_path, profile, windows, source_oracle
    )
    if (
        Path(request["sourceIdentity"]["path"]).resolve() != wav_path.resolve()
        or request["sourceIdentity"]["sampleRateHz"] != fs
    ):
        raise ValueError("Pipeline source is not the actual frozen WAV")
    decoded_windows, rows = [], []
    raw_identities = []
    for mapping, stored in windows:
        ref = mapping["headEvidence"]
        path = bounded_path(directory, ref["path"])
        digest = sha256(path)
        if (
            digest != ref["sha256"]
            or path.stat().st_size != integer(ref["bytes"])
            or ref["shape"] != [1, 3, 1, width // 4]
            or ref["headOrder"] != ["hm", "bw", "off"]
        ):
            raise ValueError("Raw head hash/shape/order identity mismatch")
        heads = np.fromfile(path, dtype="<f4").reshape(3, width // 4)
        model = mapping["modelEvidence"]
        if (
            model["modelSha256"] != request["modelSha256"]
            or model["profileId"] != name
            or model["precision"] != "FP32"
        ):
            raise ValueError(
                "Per-window model/runtime identity differs from immutable request"
            )
        if scn and (
            model["modelInputKind"] != "maximum-of-16-psd-minmax"
            or model["meanSpectrumInferenceCalls"] != 0
        ):
            raise ValueError(
                "SCN production evidence is not maximum-spectrum-only"
            )
        decoded = decode_heads(heads, request["parameters"], width, scn)
        double_geometry = decode_heads(
            heads, request["parameters"], width, scn, False
        )
        raw_stored = (
            mapping.get("scnPostprocess", {}).get("rawCandidates", stored)
            if scn
            else stored
        )
        stage = compare_candidates(
            decoded, raw_stored, "all-window-raw-head-decode"
        )
        stage["windowId"] = mapping["windowId"]
        stage["doubleGeometryReference"] = compare_candidates(
            double_geometry, raw_stored, "model-probe-double-geometry"
        )
        stage["passed"] &= stage["doubleGeometryReference"]["passed"]
        if scn:
            if "scnPostprocess" not in mapping:
                raise ValueError(
                    "SCN missing per-window CNR/fusion/refinement stage evidence"
                )
            start = integer(mapping["firstFrameIndex"]) - frames[0]
            length = int(request["profile"]["modelFrames"])
            postprocess = scn_window(
                decoded, spectra[start : start + length], mapping
            )
            stage["scnPostprocess"] = compare_scn_stages(
                postprocess,
                mapping["scnPostprocess"],
                stored,
                mapping["frequencyStepHz"],
            )
            stage["passed"] &= stage["scnPostprocess"]["passed"]
            decoded = postprocess["candidates"]
        rows.append(stage)
        decoded_windows.append((mapping, decoded))
        raw_identities.append(
            {
                "windowId": mapping["windowId"],
                "path": ref["path"],
                "sha256": digest,
                "modelEvidence": mapping.get("modelEvidence"),
            }
        )
    tracks = associate(decoded_windows, request, denominator)
    annotations = read_json(directory / "annotations.json")
    if annotations != durable_annotations:
        raise ValueError(
            "Annotation export differs from its hash-verified durable publication"
        )
    track_report = compare_tracks(
        tracks,
        annotations,
        request,
        frames,
        spectra,
        hop,
        denominator,
        scn,
        {mapping["windowId"]: mapping for mapping, _ in windows},
    )
    return {
        "profileId": name,
        "manifestSha256": sha256(manifest_path),
        "annotationSha256": sha256(directory / "annotations.json"),
        "requestModelSha256": request["modelSha256"],
        "sampleRateHz": fs,
        "eligibleWindows": denominator,
        "rawHeadIdentities": raw_identities,
        "canonicalChunkIdentities": chunk_identities,
        "decodeWindows": rows,
        "frequencyTracks": track_report,
        "passed": bool(
            all(row["passed"] for row in rows) and track_report["passed"]
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pipeline-root", required=True, type=Path)
    parser.add_argument("--assets", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument(
        "--profile", choices=("ffscn-gui-iq-v1", "scn-max-psd-v1")
    )
    arguments = parser.parse_args()
    report = {
        "schema": "signalstudio-all-window-frequency-parity-v1",
        "passed": False,
        "oracle": "Independent Python interval math and repository-owned Torch CPU WAV DSP",
        "python": sys.version,
        "numpy": np.__version__,
        "torch": torch.__version__,
        "limits": {
            "orderedEdgeBins": BIN_TOLERANCE,
            "score": SCORE_TOLERANCE,
            "noiseFloorDb": PSD_TOLERANCE_DB,
            "maximumWindows": MAX_WINDOWS,
        },
        "scope": "Actual 20ms requested ROI of the frozen full 0.8s WAV; all eligible contexts",
        "modelTimeEventsVerified": False,
        "historicalTfF1Reproduced": False,
        "mathematicalVersion": "frequency-postprocess-v1-unique-window-max-score",
        "declaredDifferences": [
            "Each unique window contributes max score across its fused fragments; GUI legacy mean could duplicate weight",
            "SCN decoded bands clip to the project [0,N-1] physical grid",
            "GUI reference float32 mean versus native float64 accumulation; existing PSD tolerance retained",
        ],
        "profiles": [],
    }
    try:
        assets = read_json(arguments.assets)
        report["oracleSha256"] = sha256(Path(__file__))
        report["m0AssetsSha256"] = sha256(arguments.assets)
        report["pipelineReportSha256"] = sha256(
            arguments.pipeline_root / "pipeline-report.json"
        )
        report["referenceSourceIdentities"] = audit_sources(assets)
        report["frozenModelIdentities"] = audit_frozen_models(assets)
        identity = assets["datasets"]["matlabGenerator"]["goldenPair"]["wav"]
        wav_path = Path(identity["path"])
        report["wavSha256"] = sha256(wav_path)
        if report["wavSha256"] != identity["sha256"]:
            raise ValueError("Actual WAV differs from the frozen M0 source")
        repository = Path(__file__).resolve().parents[2]
        oracle_path = (
            repository
            / "tests/fixtures/wideband/generate_detection_source_golden.py"
        )
        spec = importlib.util.spec_from_file_location(
            "owned_wav_oracle", oracle_path
        )
        source_oracle = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(source_oracle)
        report["ownedDspOracleSha256"] = sha256(oracle_path)
        pipeline = read_json(arguments.pipeline_root / "pipeline-report.json")
        selected = [
            profile
            for profile in pipeline["profiles"]
            if arguments.profile is None
            or profile["profileId"] == arguments.profile
        ]
        if not selected:
            raise ValueError(
                "Requested profile has no actual pipeline artifacts"
            )
        report["requestedProfiles"] = [
            profile["profileId"] for profile in selected
        ]
        for profile in selected:
            try:
                value = verify_profile(
                    arguments.pipeline_root, profile, wav_path, source_oracle
                )
            except (OSError, ValueError, KeyError, TypeError) as exception:
                value = {
                    "profileId": profile["profileId"],
                    "passed": False,
                    "error": str(exception),
                }
            report["profiles"].append(value)
        report["passed"] = all(
            profile["passed"] for profile in report["profiles"]
        )
    except (OSError, ValueError, KeyError, TypeError) as exception:
        report["error"] = str(exception)
    arguments.report.parent.mkdir(parents=True, exist_ok=True)
    arguments.report.write_text(
        json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False)
        + "\n",
        encoding="utf-8",
    )
    print(
        json.dumps(
            {
                "passed": report["passed"],
                "report": str(arguments.report),
                "profiles": [
                    {
                        "profileId": value["profileId"],
                        "passed": value["passed"],
                        "error": value.get("error"),
                    }
                    for value in report["profiles"]
                ],
            },
            ensure_ascii=False,
        )
    )
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
