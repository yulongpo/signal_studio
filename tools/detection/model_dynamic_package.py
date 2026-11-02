"""Build and probe the independent FFSCN model with dynamic input dimensions.

Only frozen epoch-1200 weights are accepted. TensorRT resources and generated
models stay outside source control. Staging replaces only the FFSCN model
directory; it does not copy or alter SCN assets or deployment dependencies.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
import time

import numpy as np
import torch

from model_ffscn import (
    CHECKPOINT_SHA256,
    OUTPUT_NAMES,
    load_model,
    sha256_file,
)
from model_probe import import_tensorrt, tensor_difference


MINIMUM = (1, 1, 1, 2048)
OPTIMUM = (2, 1, 10, 65536)
PROBE_SHAPES = ((1, 1, 1, 2048), (1, 1, 3, 8192), OPTIMUM,
                (1, 1, 32, 2048), (1, 1, 1, 131072))
ENGINE_NAME = "ffscn_epoch1200_dynamic_fp32.engine"
ONNX_NAME = "ffscn_epoch1200_dynamic.onnx"
TOLERANCE = {"atol": 1e-4, "rtol": 1e-3}


def write_json(path: Path, value: dict) -> None:
    """Publish a complete JSON file without partially replacing an old file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".staging")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n",
                         encoding="utf-8")
    temporary.replace(path)


def asset_entry(path: Path) -> dict:
    return {"path": path.name, "sha256": sha256_file(path),
            "bytes": path.stat().st_size}


def save_tensor(path: Path, value: np.ndarray) -> dict:
    """Save contiguous little-endian float32 tensors for native checks."""
    path.write_bytes(np.asarray(value, dtype="<f4", order="C").tobytes())
    return {**asset_entry(path), "shape": list(value.shape), "dtype": "<f4"}


def check_asset(directory: Path, entry: dict) -> Path:
    """Do not let a stale or traversing manifest redirect a staging copy."""
    name = entry["path"]
    if Path(name).name != name:
        raise ValueError("Model package asset must use a local file name")
    path = directory / name
    if (path.stat().st_size != entry["bytes"] or
            sha256_file(path) != entry["sha256"]):
        raise ValueError(f"Model package asset identity differs: {path}")
    return path


def export_dynamic(model, destination: Path) -> dict:
    """The graph mean accepts real time rows; FPN width aligns to 2048 bins."""
    started = time.monotonic()
    sample = torch.zeros(MINIMUM, dtype=torch.float32)
    torch.onnx.export(
        model.cpu(), sample, str(destination), input_names=["spectrum"],
        output_names=list(OUTPUT_NAMES), opset_version=18, dynamo=False,
        dynamic_axes={"spectrum": {0: "batch", 2: "time", 3: "frequency"},
                      **{name: {0: "batch", 3: "head_frequency"}
                         for name in OUTPUT_NAMES}})
    import onnx
    graph = onnx.load(destination)
    onnx.checker.check_model(graph)
    dimensions = graph.graph.input[0].type.tensor_type.shape.dim
    symbolic = [dimension.dim_param or dimension.dim_value
                for dimension in dimensions]
    if symbolic != ["batch", 1, "time", "frequency"]:
        raise ValueError(f"Export lost dynamic dimensions: {symbolic}")
    return {**asset_entry(destination), "seconds": time.monotonic() - started,
            "inputDimensions": symbolic, "opset": 18}


def build_engine(trt, onnx_path: Path, destination: Path, profiles: list,
                 workspace_mib: int, *, precision: str = "FP32") -> dict:
    """Build one explicit-batch engine. FP32 stays the production default.

    FP16 is opt-in. INT8 is never enabled; this project has no calibration set.
    """
    if precision not in ("FP32", "FP16"):
        raise ValueError("Only FP32 and explicit FP16 engines are supported")
    logger = trt.Logger(trt.Logger.WARNING)
    builder = trt.Builder(logger)
    network = builder.create_network(1 << int(
        trt.NetworkDefinitionCreationFlag.EXPLICIT_BATCH))
    parser = trt.OnnxParser(network, logger)
    if not parser.parse(onnx_path.read_bytes()):
        raise RuntimeError("; ".join(str(parser.get_error(index))
                                    for index in range(parser.num_errors)))
    config = builder.create_builder_config()
    config.set_memory_pool_limit(trt.MemoryPoolType.WORKSPACE,
                                 workspace_mib * 1024 * 1024)
    config.clear_flag(trt.BuilderFlag.TF32)
    config.clear_flag(trt.BuilderFlag.FP16)
    int8 = getattr(trt.BuilderFlag, "INT8", None)
    if int8 is not None:
        config.clear_flag(int8)
    if precision == "FP16":
        if not getattr(builder, "platform_has_fast_fp16", True):
            raise RuntimeError("TensorRT reports no FP16 support on this GPU")
        config.set_flag(trt.BuilderFlag.FP16)
    cache_path = destination.with_suffix(".timing-cache")
    cache = config.create_timing_cache(
        cache_path.read_bytes() if cache_path.exists() else b"")
    config.set_timing_cache(cache, ignore_mismatch=False)
    for dimensions in profiles:
        profile = builder.create_optimization_profile()
        shapes = [tuple(dimensions[key]) for key in
                  ("minShape", "optShape", "maxShape")]
        profile.set_shape("spectrum", *shapes)
        if [tuple(shape) for shape in profile.get_shape("spectrum")] != shapes:
            raise ValueError("TensorRT rejected the dynamic profile")
        config.add_optimization_profile(profile)
    started = time.monotonic()
    serialized = builder.build_serialized_network(network, config)
    if serialized is None:
        raise RuntimeError("TensorRT builder returned no engine")
    temporary = destination.with_name(destination.name + ".staging")
    temporary.write_bytes(bytes(serialized))
    temporary.replace(destination)
    cache_path.write_bytes(bytes(config.get_timing_cache().serialize()))
    return {**asset_entry(destination), "seconds": time.monotonic() - started,
            "tf32": False, "fp16": precision == "FP16", "precision": precision,
            "workspaceMiB": workspace_mib, "profiles": profiles}


class DynamicRunner:
    """Allocate activations for the actual input rather than profile maximum."""

    def __init__(self, trt, engine_path: Path):
        self.logger = trt.Logger(trt.Logger.WARNING)
        self.runtime = trt.Runtime(self.logger)
        self.engine = self.runtime.deserialize_cuda_engine(
            engine_path.read_bytes())
        if self.engine is None:
            raise RuntimeError("Dynamic engine deserialization failed")
        self.context = self.engine.create_execution_context(
            trt.ExecutionContextAllocationStrategy.USER_MANAGED)
        if self.context is None:
            raise RuntimeError("Dynamic engine context creation failed")
        self.maximum_activation_bytes = self.engine.device_memory_size_v2
        self.stream = torch.cuda.Stream()

    def run(self, value: torch.Tensor) -> tuple[list[np.ndarray], dict]:
        shape = tuple(value.shape)
        profile_index = None
        for index in range(self.engine.num_optimization_profiles):
            minimum, _, maximum = self.engine.get_tensor_profile_shape(
                "spectrum", index)
            if all(low <= value <= high for value, low, high in
                   zip(shape, minimum, maximum, strict=True)):
                profile_index = index
                break
        if profile_index is None:
            raise ValueError(f"No runtime profile covers actual shape: {shape}")
        stream = self.stream
        stream.wait_stream(torch.cuda.current_stream(value.device))
        if not self.context.set_optimization_profile_async(
                profile_index, stream.cuda_stream):
            raise RuntimeError("Could not select the dynamic input profile")
        if not self.context.set_input_shape("spectrum", shape):
            raise ValueError(f"Input shape is outside the package: {shape}")
        activation_bytes = self.context.update_device_memory_size_for_shapes()
        activation = torch.empty(activation_bytes + 256, dtype=torch.uint8,
                                 device=value.device)
        address = (activation.data_ptr() + 255) & ~255
        self.context.set_device_memory(address, activation_bytes)
        outputs = [torch.empty(tuple(self.context.get_tensor_shape(name)),
                               dtype=torch.float32, device=value.device)
                   for name in OUTPUT_NAMES]
        self.context.set_tensor_address("spectrum", value.data_ptr())
        for name, output in zip(OUTPUT_NAMES, outputs, strict=True):
            self.context.set_tensor_address(name, output.data_ptr())
        stream.synchronize()
        started = time.monotonic()
        if not self.context.execute_async_v3(stream.cuda_stream):
            raise RuntimeError("Dynamic TensorRT enqueue failed")
        stream.synchronize()
        return [output.cpu().numpy() for output in outputs], {
            "milliseconds": (time.monotonic() - started) * 1000,
            "activationBytes": activation_bytes,
            "profileIndex": profile_index,
            "maximumProfileActivationBytes": self.maximum_activation_bytes}


def manifest_for(engine: dict, onnx: dict, profiles: list) -> dict:
    maximum = [max(profile["maxShape"][axis] for profile in profiles)
               for axis in range(4)]
    return {
        "schema": "signalstudio-detection-model-package-v2",
        "profileId": "ffscn-gui-iq-v1", "profileVersion": 1,
        "adaptationVersion": 2, "precision": "FP32",
        "checkpointSha256": CHECKPOINT_SHA256,
        "engine": {key: engine[key] for key in ("path", "sha256", "bytes")},
        "onnx": {key: onnx[key] for key in ("path", "sha256", "bytes")},
        "runtime": {"tensorRt": "10.11.0.33", "cudaRuntime": 12090,
                    "gpu": "NVIDIA GeForce RTX 5060 Laptop GPU",
                    "computeCapability": [12, 0],
                    "hardwareCompatibility": "none",
                    "versionCompatible": False},
        "input": {"name": "spectrum", "dtype": "float32",
                  "layout": "contiguous-NCHW", "minShape": list(MINIMUM),
                  "optShape": list(OPTIMUM), "maxShape": list(maximum),
                  "downNums": 11, "frequencyPowerOfTwo": True,
                  "frequencyMultiple": 2048},
        "profiles": profiles,
        "outputStride": 4,
        "heads": [{"semantic": name, "name": name, "dtype": "float32",
                   "shape": [0, 1, 1, 0], "stride": 4}
                  for name in OUTPUT_NAMES],
        "allocationPolicy": "actual-shape-user-managed-activation",
        "boundary": ("Dynamic structure and raw-head forward parity only; "
                     "no label-based accuracy or real-time claim"),
    }


def stage_package(asset_root: Path, output_bin: Path, report: dict) -> dict:
    if not report.get("passed"):
        raise ValueError("Staging requires successful raw-head parity probes")
    manifest_path = asset_root / "manifest.json"
    if sha256_file(manifest_path) != report["manifestSha256"]:
        raise ValueError("Verified manifest was changed after its probe")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    destination = output_bin / "detection-models/ffscn-gui-iq-v1"
    destination.mkdir(parents=True, exist_ok=True)
    engine = check_asset(asset_root, manifest["engine"])
    for source in (engine, manifest_path):
        target = destination / source.name
        temporary = target.with_name(target.name + ".staging")
        shutil.copyfile(source, temporary)
        if sha256_file(temporary) != sha256_file(source):
            raise ValueError("Staged package copy differs")
        temporary.replace(target)
    return {"outputBin": str(output_bin.resolve()),
            "manifest": str((destination / "manifest.json").resolve()),
            "manifestSha256": sha256_file(destination / "manifest.json"),
            "engineSha256": sha256_file(destination / engine.name),
            "scope": "FFSCN engine and manifest only; SCN and DLLs untouched"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--trt-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--max-time-frames", type=int, default=64)
    parser.add_argument("--max-frequency-bins", type=int, default=262144)
    parser.add_argument("--wide-time-frames", type=int, default=16)
    parser.add_argument("--long-frequency-bins", type=int, default=65536)
    parser.add_argument("--workspace-mib", type=int, default=1024)
    parser.add_argument("--reuse-engine", action="store_true")
    parser.add_argument("--stage-only", action="store_true")
    parser.add_argument("--output-bin", type=Path)
    args = parser.parse_args()
    if args.stage_only:
        if args.output_bin is None:
            parser.error("--stage-only requires --output-bin")
        report = json.loads(args.report.read_text(encoding="utf-8"))
        print(json.dumps(stage_package(args.output, args.output_bin, report)))
        return 0
    maximum = (2, 1, args.max_time_frames, args.max_frequency_bins)
    if (maximum[2] < OPTIMUM[2] or maximum[3] < OPTIMUM[3] or
            maximum[3] % 2048 or args.workspace_mib < 1 or
            args.wide_time_frames < OPTIMUM[2] or
            args.wide_time_frames > maximum[2] or
            args.long_frequency_bins < OPTIMUM[3] or
            args.long_frequency_bins > maximum[3] or
            args.long_frequency_bins % 2048):
        parser.error("Profile maxima must include opt and align frequency")
    profiles = [
        {"index": 0, "minShape": list(MINIMUM), "optShape": list(OPTIMUM),
         "maxShape": [2, 1, maximum[2], args.long_frequency_bins]},
        {"index": 1, "minShape": list(MINIMUM),
         "optShape": [1, *OPTIMUM[1:]],
         "maxShape": [2, 1, args.wide_time_frames, maximum[3]]},
    ]
    args.output.mkdir(parents=True, exist_ok=True)
    onnx_path = args.output / ONNX_NAME
    engine_path = args.output / ENGINE_NAME
    metadata_path = args.output / "build-metadata.json"
    graph_source = Path(__file__).with_name("model_ffscn.py")
    identity = {"checkpointSha256": CHECKPOINT_SHA256,
                "graphSourceSha256": sha256_file(graph_source),
                "minShape": list(MINIMUM), "optShape": list(OPTIMUM),
                "maxShape": list(maximum), "tf32": False,
                "profiles": profiles,
                "precision": "FP32", "opset": 18}
    report = {"schema": "signalstudio-dynamic-ffscn-probe-v1",
              "identity": identity, "tolerance": TOLERANCE,
              "assetRoot": str(args.output.resolve()),
              "scriptSha256": sha256_file(Path(__file__)),
              "claim": "Structure and raw head parity, not accuracy",
              "timingScope": "Single synchronized forward, not cold/hot timing",
              "memoryScope": "Torch allocations, not total process VRAM",
              "engineReused": args.reuse_engine,
              "passed": False}
    started = time.monotonic()
    try:
        if sha256_file(args.checkpoint) != CHECKPOINT_SHA256:
            raise ValueError("Checkpoint differs from frozen epoch-1200")
        torch.backends.cuda.matmul.allow_tf32 = False
        torch.backends.cudnn.allow_tf32 = False
        torch.set_float32_matmul_precision("highest")
        model = load_model(args.checkpoint, "cpu")
        trt, dll_handles = import_tensorrt(args.trt_root)
        if str(trt.__version__) != "10.11.0.33":
            raise ValueError("TensorRT differs from frozen deployment runtime")
        if args.reuse_engine:
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
            if metadata["identity"] != identity:
                raise ValueError("Cached engine build inputs differ")
            check_asset(args.output, metadata["onnx"])
            check_asset(args.output, metadata["engine"])
        else:
            onnx = export_dynamic(model, onnx_path)
            print("Dynamic ONNX exported", flush=True)
            engine = build_engine(trt, onnx_path, engine_path, profiles,
                                  args.workspace_mib)
            metadata = {"identity": identity, "onnx": onnx, "engine": engine}
            write_json(metadata_path, metadata)
            print("Dynamic FP32 engine built", flush=True)
        report["build"] = metadata
        manifest = manifest_for(metadata["engine"], metadata["onnx"], profiles)
        manifest_path = args.output / "manifest.json"
        write_json(manifest_path, manifest)
        report["manifestSha256"] = sha256_file(manifest_path)
        report["runtime"] = {"pytorch": torch.__version__,
                             "tensorRt": str(trt.__version__),
                             "gpu": torch.cuda.get_device_name(0),
                             "computeCapability": list(
                                 torch.cuda.get_device_capability(0))}
        model.cuda()
        runner = DynamicRunner(trt, engine_path)
        probes = []
        generator = np.random.default_rng(20261015)
        probe_shapes = [shape for shape in PROBE_SHAPES
                        if any(all(low <= value <= high
                                   for value, low, high in zip(
                                       shape, profile["minShape"],
                                       profile["maxShape"], strict=True))
                               for profile in profiles)]
        for shape in probe_shapes:
            values = generator.standard_normal(shape, dtype=np.float32)
            value = torch.from_numpy(values).cuda()
            torch.cuda.reset_peak_memory_stats()
            with torch.inference_mode():
                expected = [head.cpu().numpy() for head in model(value)]
            if not all(np.isfinite(head).all() for head in expected):
                raise ValueError("PyTorch reference contains nonfinite values")
            actual, timing = runner.run(value)
            stem = f"shape-b{shape[0]}-t{shape[2]}-f{shape[3]}"
            tensors = {"input": save_tensor(
                args.output / f"{stem}.input.f32", values)}
            tensors.update({name: save_tensor(
                args.output / f"{stem}.{name}.reference.f32", head)
                            for name, head in
                            zip(OUTPUT_NAMES, expected, strict=True)})
            heads = {name: tensor_difference(reference, output)
                     for name, reference, output in
                     zip(OUTPUT_NAMES, expected, actual, strict=True)}
            probe = {"shape": list(shape), "seed": 20261015,
                     "inputSha256": hashlib.sha256(
                         values.tobytes()).hexdigest(),
                     "heads": heads, "tensors": tensors, "inference": timing,
                     "torchPeakAllocatedBytes":
                         torch.cuda.max_memory_allocated(),
                     "passed": all(head["passed"] for head in heads.values())}
            probes.append(probe)
            print(f"Probe {list(shape)}: passed={probe['passed']}", flush=True)
            del value
        report["probes"] = probes
        report["passed"] = all(probe["passed"] for probe in probes)
        report["seconds"] = time.monotonic() - started
        write_json(args.report, report)
        if args.output_bin:
            report["staging"] = stage_package(args.output, args.output_bin,
                                               report)
            write_json(args.report, report)
        del dll_handles
        return 0 if report["passed"] else 1
    except Exception as error:
        report["error"] = f"{type(error).__name__}: {error}"
        report["seconds"] = time.monotonic() - started
        write_json(args.report, report)
        raise


if __name__ == "__main__":
    sys.exit(main())
