"""Export the independent model and verify named outputs on real GPU runtimes.

Run this tool with a development Python containing PyTorch, TorchVision, ONNX,
ONNX Runtime and ONNXScript. TensorRT is staged separately below --trt-root.
Assets remain under the ignored out/tasks/wideband-detection/M3/assets tree; only reports are committed.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import sys
import time
import wave

import numpy as np
import torch

from model_ffscn import (
    CHECKPOINT_SHA256,
    INPUT_SHAPE,
    OUTPUT_NAMES,
    load_model,
    sha256_file,
)


TOLERANCE = {"atol": 1e-4, "rtol": 1e-3}


def frame_spectra(iq: torch.Tensor, sample_rate: int) -> torch.Tensor:
    """Periodic-Hann density spectra at frozen 1 ms, centered FFT supports.

    This implements only full-band complex IQ. Partial FFT supports at the
    file edges are explicitly zero-padded; ten real time slots are required.
    """
    hop = int(sample_rate * 0.001)
    nfft = INPUT_SHAPE[-1]
    if hop < 1 or iq.numel() // hop < 10:
        raise ValueError("Input has fewer than ten complete 1 ms time slots")
    slot_count = iq.numel() // hop
    window = torch.hann_window(nfft, periodic=True, device=iq.device)
    if hop < nfft:
        extra = nfft - hop
        remainder = iq.numel() - slot_count * hop
        if extra > remainder:
            padding = extra - remainder
            iq = torch.nn.functional.pad(
                iq, (padding // 2, padding - padding // 2))
        else:
            iq = iq[:slot_count * hop + extra]
        records = iq.unfold(0, nfft, hop)
    else:
        slots = iq.unfold(0, hop, hop)
        padding = (-hop) % nfft
        slots = torch.nn.functional.pad(
            slots, (padding // 2, padding - padding // 2))
        records = slots.unfold(1, nfft, nfft)
    transformed = torch.fft.fftshift(
        torch.fft.fft(records * window, dim=-1), dim=-1)
    power = transformed.abs().square()
    if power.ndim == 3:
        power = power.mean(dim=1)
    tiny = torch.finfo(power.dtype).tiny
    # Follow the reference power floor, but perform division/log in double so
    # a zero-power frame cannot underflow during density normalization.
    density = power.double().clamp_min(tiny) / (
        sample_rate * window.square().sum().double())
    return (10 * torch.log10(density)).to(power.dtype)


def normalized_windows(spectra: torch.Tensor) -> torch.Tensor:
    """Batch the first two real contexts using global sample std, ddof=1."""
    starts = [0, min(5, spectra.shape[0] - 10)]
    normalized = []
    for start in starts:
        window = spectra[start:start + 10]
        if not torch.isfinite(window).all():
            raise ValueError("Nonfinite PSD input")
        deviation = window.std(correction=1).clamp_min(
            torch.finfo(window.dtype).eps)
        normalized.append((window - window.mean()) / deviation)
    return torch.stack(normalized).unsqueeze(1)


def real_wav_tensor(path: Path, device: str) -> tuple[torch.Tensor, dict]:
    """Read a bounded genuine PCM16 I/Q prefix, never generated test samples."""
    with wave.open(str(path), "rb") as stream:
        if (stream.getnchannels(), stream.getsampwidth(),
                stream.getcomptype()) != (2, 2, "NONE"):
            raise ValueError("Expected two-channel PCM16 I/Q WAV")
        sample_rate = stream.getframerate()
        count = min(stream.getnframes(), int(sample_rate * 0.02))
        raw = stream.readframes(count)
    components = np.frombuffer(raw, dtype="<i2").reshape(-1, 2)
    complex_iq = (components[:, 0].astype(np.float32) +
                  1j * components[:, 1].astype(np.float32)) / 32768.0
    spectra = frame_spectra(torch.from_numpy(complex_iq).to(device), sample_rate)
    metadata = {
        "path": str(path.resolve()), "sha256": sha256_file(path),
        "sampleRateHz": sample_rate, "prefixSamples": count,
        "frameCount": spectra.shape[0], "fft": INPUT_SHAPE[-1],
        "stftHopSamples": int(sample_rate * 0.001),
        "paddingPolicy": "centered zero support at bounded prefix edges",
        "dbFloor": "float32 power tiny, double density/log, cast to float32",
        "numericalVersion": "reference-power-floor-double-log-v1",
        "zeroInputBoundary": "Explicit double-log guard avoids reference underflow -inf",
        "scope": "20 ms prefix used for deployment tensor parity only",
    }
    return normalized_windows(spectra), metadata


def synthetic_tensor(device: str) -> torch.Tensor:
    """Deterministic noise and narrow/wide spectral features; not accuracy data."""
    rng = np.random.default_rng(20261013)
    values = rng.standard_normal((2, *INPUT_SHAPE[1:]), dtype=np.float32)
    values[0, :, 2:7, 20000:20200] += 5
    values[0, :, 1:9, 41000:43500] += 3
    values[1, :, 0:6, 200:260] += 4
    return torch.from_numpy(values).to(device)


def tensor_difference(reference: np.ndarray, actual: np.ndarray) -> dict:
    if reference.shape != actual.shape:
        raise ValueError(f"Output shape mismatch: {reference.shape}, {actual.shape}")
    difference = np.abs(reference - actual)
    limits = TOLERANCE["atol"] + TOLERANCE["rtol"] * np.abs(reference)
    return {
        "shape": list(actual.shape), "dtype": str(actual.dtype),
        "finite": bool(np.isfinite(actual).all()),
        "maxAbsolute": float(difference.max()),
        "meanAbsolute": float(difference.mean()),
        "maxRelative": float((difference /
                              np.maximum(np.abs(reference), 1e-12)).max()),
        "outsideTolerance": int(np.count_nonzero(difference > limits)),
        "elements": actual.size,
        "passed": bool(np.isfinite(actual).all() and
                       not np.any(difference > limits)),
    }


def decode(outputs: list[np.ndarray]) -> list[list[list[float]]]:
    """Frozen seven-bin local peaks and standard kept-candidate-only 1-D NMS."""
    heatmap, bandwidth, offset = (torch.from_numpy(value) for value in outputs)
    local = torch.nn.functional.max_pool2d(
        heatmap, kernel_size=(1, 7), stride=1, padding=(0, 3))
    results = []
    for batch in range(heatmap.shape[0]):
        scores = heatmap[batch, 0, 0]
        indices = torch.nonzero((scores == local[batch, 0, 0]) &
                                (scores > 0.7)).flatten().tolist()
        indices.sort(key=lambda index: (-float(scores[index]), index))
        kept = []
        for index in indices[:512]:
            width = float(bandwidth[batch, 0, 0, index]) * 4
            center = (index + float(offset[batch, 0, 0, index])) * 4
            if width <= 0:
                continue
            lower = max(0.0, center - width / 2)
            upper = min(float(INPUT_SHAPE[-1] - 1), center + width / 2)
            if upper <= lower:
                continue
            suppressed = False
            for previous in kept:
                intersection = max(0.0, min(upper, previous[1]) -
                                   max(lower, previous[0]))
                union = upper - lower + previous[1] - previous[0] - intersection
                if intersection / union > 0.3:
                    suppressed = True
                    break
            if not suppressed:
                kept.append([lower, upper, float(scores[index])])
        results.append(kept)
    return results


def candidate_difference(reference: list, actual: list) -> dict:
    counts_match = [len(value) for value in reference] == [
        len(value) for value in actual]
    max_edge = 0.0
    max_score = 0.0
    if counts_match:
        for expected, observed in zip(reference, actual, strict=True):
            for first, second in zip(expected, observed, strict=True):
                max_edge = max(max_edge, abs(first[0] - second[0]),
                               abs(first[1] - second[1]))
                max_score = max(max_score, abs(first[2] - second[2]))
    return {
        "referenceCounts": [len(value) for value in reference],
        "actualCounts": [len(value) for value in actual],
        "countsMatch": counts_match,
        "maxOrderedEdgeBins": max_edge if counts_match else None,
        "maxOrderedScore": max_score if counts_match else None,
        "passed": counts_match and max_edge <= 0.05 and max_score <= 0.001,
        "note": "Ordered decoded candidates; not label-based accuracy",
    }


def import_tensorrt(root: Path):
    sys.path.insert(0, str(root.resolve()))
    handles = []
    for subpath in ("tensorrt_libs", "nvidia/cuda_runtime/bin"):
        directory = root / subpath
        if directory.is_dir() and os.name == "nt":
            handles.append(os.add_dll_directory(str(directory.resolve())))
    import tensorrt
    return tensorrt, handles


def build_engine(trt, onnx_path: Path, engine_path: Path,
                 half_precision: bool) -> dict:
    logger = trt.Logger(trt.Logger.WARNING)
    builder = trt.Builder(logger)
    network = builder.create_network(1 << int(
        trt.NetworkDefinitionCreationFlag.EXPLICIT_BATCH))
    parser = trt.OnnxParser(network, logger)
    if not parser.parse(onnx_path.read_bytes()):
        raise RuntimeError("; ".join(str(parser.get_error(index))
                                    for index in range(parser.num_errors)))
    config = builder.create_builder_config()
    config.set_memory_pool_limit(trt.MemoryPoolType.WORKSPACE, 1 << 30)
    config.clear_flag(trt.BuilderFlag.TF32)
    if half_precision:
        config.set_flag(trt.BuilderFlag.FP16)
    profile = builder.create_optimization_profile()
    profile.set_shape("spectrum", INPUT_SHAPE, (2, *INPUT_SHAPE[1:]),
                      (2, *INPUT_SHAPE[1:]))
    config.add_optimization_profile(profile)
    started = time.monotonic()
    serialized = builder.build_serialized_network(network, config)
    if serialized is None:
        raise RuntimeError("TensorRT builder returned no engine")
    engine_path.write_bytes(bytes(serialized))
    return {"seconds": time.monotonic() - started,
            "sha256": sha256_file(engine_path),
            "bytes": engine_path.stat().st_size,
            "precision": "FP16 candidate" if half_precision else "FP32",
            "tf32": False, "workspaceBytes": 1 << 30,
            "hardwareCompatibility": "none; build GPU specific",
            "versionCompatible": False}


class TensorRtRunner:
    """TensorRT 10 tensor API; PyTorch supplies CUDA memory and stream."""

    def __init__(self, trt, engine_path: Path):
        self.logger = trt.Logger(trt.Logger.WARNING)
        self.runtime = trt.Runtime(self.logger)
        self.engine = self.runtime.deserialize_cuda_engine(
            engine_path.read_bytes())
        if self.engine is None:
            raise RuntimeError("Engine deserialization failed")
        self.context = self.engine.create_execution_context()
        if self.context is None:
            raise RuntimeError("Engine context creation failed")

    def __call__(self, value: torch.Tensor) -> tuple[list[np.ndarray], float]:
        if not self.context.set_input_shape("spectrum", tuple(value.shape)):
            raise RuntimeError("Input shape is outside the engine profile")
        output_tensors = []
        for name in OUTPUT_NAMES:
            shape = tuple(self.context.get_tensor_shape(name))
            output_tensors.append(torch.empty(shape, device=value.device,
                                              dtype=torch.float32))
        self.context.set_tensor_address("spectrum", value.data_ptr())
        for name, output in zip(OUTPUT_NAMES, output_tensors, strict=True):
            self.context.set_tensor_address(name, output.data_ptr())
        stream = torch.cuda.current_stream(value.device)
        stream.synchronize()
        started = time.monotonic()
        if not self.context.execute_async_v3(stream.cuda_stream):
            raise RuntimeError("TensorRT enqueue failed")
        stream.synchronize()
        elapsed = (time.monotonic() - started) * 1000
        return [output.cpu().numpy() for output in output_tensors], elapsed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--wav", type=Path, required=True)
    parser.add_argument("--trt-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("out/tasks/wideband-detection/M3/assets/epoch1200"))
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--reuse-engines", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    torch.backends.cuda.matmul.allow_tf32 = False
    torch.backends.cudnn.allow_tf32 = False
    model = load_model(args.checkpoint, "cuda")
    real_input, real_metadata = real_wav_tensor(args.wav, "cuda")
    probes = {"synthetic": synthetic_tensor("cuda"), "real-wav": real_input,
              "constant": torch.zeros((2, *INPUT_SHAPE[1:]), device="cuda")}
    onnx_path = args.output / "ffscn_epoch1200.onnx"
    if not args.reuse_engines:
        torch.onnx.export(
            model.cpu(), real_input.cpu(), str(onnx_path),
            input_names=["spectrum"], output_names=list(OUTPUT_NAMES),
            opset_version=18, dynamo=False,
            dynamic_axes={"spectrum": {0: "batch"},
                          **{name: {0: "batch"} for name in OUTPUT_NAMES}})
        model.cuda()
    import onnx
    onnx.checker.check_model(onnx.load(onnx_path))
    import onnxruntime
    session = onnxruntime.InferenceSession(
        str(onnx_path), providers=["CPUExecutionProvider"])
    trt, dll_handles = import_tensorrt(args.trt_root)
    previous_engines = {}
    if args.reuse_engines and args.report.exists():
        previous_engines = json.loads(
            args.report.read_text(encoding="utf-8")).get("engines", {})
    engines = {}
    runners = {}
    for precision, half in (("fp32", False), ("fp16", True)):
        engine_path = args.output / f"ffscn_epoch1200_{precision}.engine"
        engines[precision] = (build_engine(trt, onnx_path, engine_path, half)
                              if not args.reuse_engines else
                              {**previous_engines.get(precision, {}),
                               "sha256": sha256_file(engine_path),
                               "bytes": engine_path.stat().st_size,
                               "reused": True})
        runners[precision] = TensorRtRunner(trt, engine_path)
        print(f"Built/loaded {precision}: {engines[precision]}", flush=True)
    report = {
        "schema": "signalstudio-model-parity-v1", "profileId": "ffscn-gui-iq-v1",
        "checkpointSha256": CHECKPOINT_SHA256,
        "modelImplementationSha256": sha256_file(Path(__file__).with_name(
            "model_ffscn.py")), "parameters": sum(
                value.numel() for value in model.parameters()),
        "onnxSha256": sha256_file(onnx_path), "onnxOpset": 18,
        "environment": {"python": sys.version, "os": platform.platform(),
                        "torch": torch.__version__, "onnx": onnx.__version__,
                        "onnxRuntime": onnxruntime.__version__,
                        "tensorRt": trt.__version__,
                        "torchCuda": torch.version.cuda,
                        "gpu": torch.cuda.get_device_name(),
                        "computeCapability": list(torch.cuda.get_device_capability())},
        "toleranceFrozenBeforeRun": TOLERANCE, "realInput": real_metadata,
        "engines": engines, "probes": [],
        "boundary": "Runtime parity, not TF accuracy or native UI acceptance",
    }
    with torch.inference_mode():
        for kind, tensor in probes.items():
            for batch in (1, 2):
                value = tensor[:batch].contiguous()
                torch.cuda.synchronize()
                started = time.monotonic()
                expected = [item.cpu().numpy() for item in model(value)]
                elapsed = (time.monotonic() - started) * 1000
                reference_candidates = decode(expected)
                probe = {"kind": kind, "batch": batch,
                         "inputSha256": hashlib_bytes(value.cpu().numpy()),
                         "torchMs": elapsed, "runtimes": {}}
                # Frozen tensors also feed the native C++ worker probe.
                prefix = f"{kind}-b{batch}"
                value.cpu().numpy().tofile(args.output / f"{prefix}.input.f32")
                for name, output in zip(OUTPUT_NAMES, expected, strict=True):
                    output.tofile(args.output / f"{prefix}.{name}.reference.f32")
                for runtime_name in ("onnx-cpu", "fp32", "fp16"):
                    started = time.monotonic()
                    if runtime_name == "onnx-cpu":
                        actual = session.run(list(OUTPUT_NAMES),
                                             {"spectrum": value.cpu().numpy()})
                        runtime_ms = (time.monotonic() - started) * 1000
                    else:
                        actual, runtime_ms = runners[runtime_name](value)
                    differences = {name: tensor_difference(first, second)
                                   for name, first, second in zip(
                                       OUTPUT_NAMES, expected, actual, strict=True)}
                    candidates = candidate_difference(reference_candidates,
                                                      decode(actual))
                    probe["runtimes"][runtime_name] = {
                        "ms": runtime_ms, "tensors": differences,
                        "candidates": candidates,
                        "passed": all(item["passed"] for item in
                                      differences.values()) and candidates["passed"]}
                report["probes"].append(probe)
                print(f"Probe {prefix}: " + str({name: item["passed"] for name, item
                      in probe["runtimes"].items()}), flush=True)
    report["fp32Passed"] = all(probe["runtimes"]["fp32"]["passed"]
                                for probe in report["probes"])
    report["fp16Passed"] = all(probe["runtimes"]["fp16"]["passed"]
                                for probe in report["probes"])
    manifest = {
        "schemaVersion": 1, "profileId": "ffscn-gui-iq-v1",
        "profileVersion": 1, "checkpointSha256": CHECKPOINT_SHA256,
        "onnx": {"path": onnx_path.name, "sha256": report["onnxSha256"]},
        "runtime": {"tensorRt": trt.__version__,
                    "gpu": report["environment"]["gpu"],
                    "computeCapability": report["environment"]["computeCapability"],
                    "hardwareCompatibility": "none",
                    "versionCompatible": False},
        "input": {"name": "spectrum", "dtype": "float32",
                  "layout": "contiguous-NCHW", "minShape": list(INPUT_SHAPE),
                  "optShape": [2, *INPUT_SHAPE[1:]],
                  "maxShape": [2, *INPUT_SHAPE[1:]],
                  "stridesElements": [655360, 655360, 65536, 1]},
        "outputs": [{"name": name, "dtype": "float32",
                     "shape": ["batch", 1, 1, 16384],
                     "stridesElements": [16384, 16384, 16384, 1]}
                    for name in OUTPUT_NAMES],
        "engines": [{"path": f"ffscn_epoch1200_{precision}.engine",
                     "sha256": identity["sha256"], "bytes": identity["bytes"],
                     "precision": precision.upper(),
                     "status": "tensor-and-candidate-parity-passed" if
                         report[f"{precision}Passed"] else "experimental"}
                    for precision, identity in engines.items()],
        "defaultPrecision": "FP32" if report["fp32Passed"] else None,
        "validationScope": "Three probe kinds, B1/B2; no accuracy guarantee",
    }
    (args.output / "ffscn_epoch1200.manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    # Keep DLL handles alive through the final context/engine destruction.
    del runners
    del dll_handles
    return 0 if report["fp32Passed"] else 1


def hashlib_bytes(value: np.ndarray) -> str:
    import hashlib
    return hashlib.sha256(value.tobytes()).hexdigest()


if __name__ == "__main__":
    raise SystemExit(main())
