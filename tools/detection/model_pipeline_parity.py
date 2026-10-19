"""Verify actual native source tensors and production-worker heads independently.

The source oracle is repository-owned Torch CPU DSP. The model oracle uses the
frozen checkpoint and an independently defined graph, never GUI business code.
Source tolerances are the M2 frozen values; L2 compares identical input bytes.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import wave

import numpy as np
import torch

from model_controls import ControlEngine
from model_ffscn import load_model, sha256_file
from model_probe import (
    candidate_difference,
    decode,
    import_tensorrt,
    tensor_difference,
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pipeline-root", type=Path, required=True)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--trt-root", type=Path, required=True)
    parser.add_argument("--control-engine", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    repository = Path(__file__).resolve().parents[2]
    assets = json.loads(
        (
            repository / "docs/implementation/wideband-detection/M0/assets.json"
        ).read_text(encoding="utf-8")
    )
    identity = assets["datasets"]["matlabGenerator"]["goldenPair"]["wav"]
    wav_path = Path(identity["path"])
    if sha256_file(wav_path) != identity["sha256"]:
        raise ValueError("Frozen source WAV identity changed")
    spec = importlib.util.spec_from_file_location(
        "source_oracle",
        repository
        / "tests/fixtures/wideband/generate_detection_source_golden.py",
    )
    oracle = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(oracle)
    pipeline = json.loads(
        (args.pipeline_root / "pipeline-report.json").read_text(
            encoding="utf-8"
        )
    )
    with wave.open(str(wav_path), "rb") as stream:
        fs, count = stream.getframerate(), stream.getnframes()
    torch.backends.cuda.matmul.allow_tf32 = False
    torch.backends.cudnn.allow_tf32 = False
    model = load_model(args.checkpoint, "cuda")
    trt, dll_handles = import_tensorrt(args.trt_root)
    scn = ControlEngine(trt, args.control_engine)
    reports = []
    for profile in pipeline["profiles"]:
        name = profile["profileId"]
        is_scn = name == "scn-max-psd-v1"
        width, frames = (32768, 16) if is_scn else (65536, 10)
        directory = args.pipeline_root / name
        actual_input = np.fromfile(
            directory / "first-input.f32", dtype="<f4"
        ).reshape(frames, width)
        actual_density = np.fromfile(
            directory / "first-density.f32", dtype="<f4"
        ).reshape(frames, width)
        first = int(profile["firstFrameIndex"])
        leading = int(profile["sourceLeadingPadding"])
        hann = torch.hann_window(width, periodic=True, dtype=torch.float32)
        spectra = torch.stack(
            [
                oracle.frame_density(
                    wav_path,
                    index,
                    count,
                    int(fs * 0.001),
                    width,
                    leading,
                    hann,
                    fs,
                )
                for index in range(first, first + frames)
            ]
        )
        expected_input = (
            spectra
            if is_scn
            else (
                (spectra - spectra.mean())
                / spectra.std(correction=1).clamp_min(
                    torch.finfo(torch.float32).eps
                )
            )
        )
        density_error = float(np.max(np.abs(actual_density - spectra.numpy())))
        input_error = float(
            np.max(np.abs(actual_input - expected_input.numpy()))
        )
        source_pass = (
            np.isfinite(actual_input).all()
            and density_error < 0.02
            and input_error < (0.02 if is_scn else 0.002)
        )
        native = np.fromfile(
            directory / "first-heads.f32", dtype="<f4"
        ).reshape(3, width // 4)
        value = torch.from_numpy(actual_input.copy()).cuda()
        if is_scn:
            maximum = value.max(dim=0).values
            normalized = (maximum - maximum.min()) / (
                maximum.max() - maximum.min()
            ).clamp_min(torch.finfo(torch.float32).eps)
            raw = scn(normalized.reshape(1, width, 1, 1).contiguous())
            expected = [
                raw[1].reshape(-1),
                raw[0].reshape(-1),
                raw[2].reshape(-1),
            ]
        else:
            with torch.inference_mode():
                expected = [
                    head.cpu().numpy().reshape(-1)
                    for head in model(value.reshape(1, 1, frames, width))
                ]
        head_reports = {
            head: tensor_difference(reference, actual)
            for head, reference, actual in zip(
                ("hm", "bw", "off"), expected, native, strict=True
            )
        }
        candidates = None
        if not is_scn:
            expected_heads = [array.reshape(1, 1, 1, -1) for array in expected]
            actual_heads = [array.reshape(1, 1, 1, -1) for array in native]
            candidates = candidate_difference(
                decode(expected_heads), decode(actual_heads)
            )
        passed = bool(
            source_pass
            and all(value["passed"] for value in head_reports.values())
            and (candidates is None or candidates["passed"])
        )
        reports.append(
            {
                "profileId": name,
                "source": {
                    "oracle": "Torch CPU actual WAV independent DSP",
                    "densityMaxErrorDb": density_error,
                    "inputMaxError": input_error,
                    "densityToleranceDb": 0.02,
                    "normalizationTolerance": 0.02 if is_scn else 0.002,
                    "allElementsCompared": True,
                    "passed": bool(source_pass),
                },
                "modelOracle": (
                    "Python TensorRT same frozen engine; maximum-only"
                    if is_scn
                    else "Independent PyTorch CUDA frozen epoch-1200 graph"
                ),
                "sameInputSha256": sha256_file(directory / "first-input.f32"),
                "heads": head_reports,
                "candidates": candidates,
                "passed": passed,
            }
        )
    report = {
        "schema": "signalstudio-native-pipeline-parity-v1",
        "wavSha256": identity["sha256"],
        "checkpointSha256": sha256_file(args.checkpoint),
        "profiles": reports,
        "passed": all(item["passed"] for item in reports),
        "boundary": "L1 full first-window tensors and L2 identical-input heads; no time-event accuracy claim",
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_bytes(
        (json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode(
            "utf-8"
        )
    )
    print(
        json.dumps(
            {"passed": report["passed"], "profiles": reports},
            ensure_ascii=False,
        )
    )
    del scn, dll_handles
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
