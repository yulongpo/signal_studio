"""Convert the frozen FFSCN checkpoint to a standalone TensorRT engine.

The Qt dialog spawns this script with the project Python. It reuses
model_ffscn and model_dynamic_package; it does not copy the network and it
does not stage anything into detection-models. The batch search is a runtime
capability probe of setInputShape / enqueue, not a throughput benchmark and
not a detection-accuracy result.
"""

from __future__ import annotations

import argparse
import gc
import json
import sys
import traceback
from pathlib import Path

SCHEMA = "signalstudio-ffscn-engine-tool-report-v1"
BATCHES = (1, 2, 4, 8, 16)
EXPECTED_TRT = "10.11.0.33"
EXPECTED_GPU = "NVIDIA GeForce RTX 5060 Laptop GPU"
FROZEN_CHANNELS = (24, 40, 112) + (80,) * 6
CLAIM = (
    "Runtime capability probe of setInputShape and enqueue for a fixed "
    "T and F. Not a throughput benchmark and not an end-to-end detection "
    "speed or accuracy result."
)
SOURCE_DIRECTORIES = {
    "app", "ui", "domain", "infrastructure", "tests", "tools", "docs",
    "reference", "cmake", "resources",
}


def write_json(path: Path, value: dict) -> None:
    """Publish a complete JSON file without partially replacing an old file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".staging")
    temporary.write_text(
        json.dumps(value, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8")
    temporary.replace(path)


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def refusal_reason(path: Path) -> str | None:
    """Keep tool output away from the deployed package and the source tree."""
    resolved = path.resolve()
    parts = [part.lower() for part in resolved.parts]
    if "detection-models" in parts:
        return "拒绝写入 detection-models 目录。"
    if resolved.name.lower() == "ffscn-gui-iq-v1":
        return "拒绝写入已部署的 ffscn-gui-iq-v1 目录。"
    try:
        relative = resolved.relative_to(repo_root())
    except ValueError:
        return None
    if not relative.parts:
        return "拒绝把引擎写入仓库根目录。"
    head = relative.parts[0].lower()
    if head in SOURCE_DIRECTORIES:
        return f"拒绝把引擎写入源码目录 {head}。"
    return None


def frequency_error(bins: int) -> str | None:
    if bins < 2048 or bins > 262144 or bins & (bins - 1):
        return "频率点数 F 必须是 2048 到 262144 之间的 2 的幂。"
    return None


def preflight(args: argparse.Namespace) -> list[str]:
    errors = []
    if not args.checkpoint.is_file():
        errors.append(f"未找到 checkpoint：{args.checkpoint}")
    if not args.trt_root.is_dir():
        errors.append(f"未找到 TensorRT 目录：{args.trt_root}")
    for label, path in (("输出目录", args.output), ("报告", args.report)):
        reason = refusal_reason(path)
        if reason:
            errors.append(f"{label}：{reason}")
    if args.time_frames < 1 or args.time_frames > 8192:
        errors.append("时间帧 T 必须在 1 到 8192 之间。")
    frequency = frequency_error(args.frequency_bins)
    if frequency:
        errors.append(frequency)
    if args.precision not in ("FP32", "FP16"):
        errors.append("精度只能是 FP32 或显式 FP16，不支持 INT8。")
    if args.workspace_mib < 1:
        errors.append("workspace 至少为 1 MiB。")
    return errors


def base_report(args: argparse.Namespace) -> dict:
    return {
        "schema": SCHEMA,
        "claim": CLAIM,
        "downNums": 11,
        "precision": args.precision,
        "timeFrames": args.time_frames,
        "frequencyBins": args.frequency_bins,
        "batchCandidates": list(BATCHES),
        "productionPackageReplaced": False,
        "installedIntoDetectionModels": False,
        "checkpoint": str(args.checkpoint),
        "trtRoot": str(args.trt_root),
        "output": str(args.output),
        "built": False,
        "ok": False,
    }


def select_inference_device(torch_module) -> dict:
    """Use the RTX 5060 only. The AMD display GPU is not a CUDA target."""
    if not torch_module.cuda.is_available():
        raise RuntimeError(
            "PyTorch 看不到 CUDA 设备，不能使用 AMD 显示 GPU 构建或运行这个引擎。")
    visible = []
    chosen = None
    for index in range(torch_module.cuda.device_count()):
        name = torch_module.cuda.get_device_name(index)
        capability = [int(value) for value in
                      torch_module.cuda.get_device_capability(index)]
        visible.append({"index": index, "name": name,
                        "computeCapability": capability})
        if name == EXPECTED_GPU and capability == [12, 0] and chosen is None:
            chosen = index
    if chosen is None:
        raise RuntimeError(
            "未找到 NVIDIA GeForce RTX 5060 Laptop GPU（SM 12.0）。"
            f"可见 CUDA 设备：{visible}。已拒绝其它 GPU。")
    torch_module.cuda.set_device(chosen)
    return {"index": chosen, "name": EXPECTED_GPU,
            "computeCapability": [12, 0], "visible": visible}


def import_helpers():
    root = Path(__file__).resolve().parent
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))
    from model_dynamic_package import (DynamicRunner, build_engine,
                                       export_dynamic)
    from model_ffscn import CHECKPOINT_SHA256, OUTPUT_NAMES, load_model
    from model_probe import import_tensorrt
    return {
        "DynamicRunner": DynamicRunner,
        "build_engine": build_engine,
        "export_dynamic": export_dynamic,
        "CHECKPOINT_SHA256": CHECKPOINT_SHA256,
        "OUTPUT_NAMES": OUTPUT_NAMES,
        "load_model": load_model,
        "import_tensorrt": import_tensorrt,
    }


def prepare_torch(report: dict):
    import torch
    torch.backends.cuda.matmul.allow_tf32 = False
    torch.backends.cudnn.allow_tf32 = False
    torch.set_float32_matmul_precision("highest")
    device = select_inference_device(torch)
    report["device"] = device
    report["pytorch"] = torch.__version__
    report["torchCuda"] = torch.version.cuda
    return torch


def load_frozen_model(helpers, checkpoint: Path):
    print("正在加载冻结的 epoch-1200 FFSCN（down_nums=11）", flush=True)
    model = helpers["load_model"](checkpoint, "cpu")
    channels = tuple(model.backbone.out_channels)
    if channels != FROZEN_CHANNELS:
        raise RuntimeError(f"FFSCN backbone 不是 down_nums=11：{channels}")
    return model


def assert_float_io(trt, engine, output_names) -> None:
    for name in ("spectrum", *output_names):
        dtype = engine.get_tensor_dtype(name)
        if dtype != trt.DataType.FLOAT:
            raise RuntimeError(
                f"引擎张量 {name} 的 dtype 是 {dtype}，本探测只接受 float32 输入输出。")


def probe_batches(runner, torch_module, report: dict, report_path: Path,
                  time_frames: int, frequency_bins: int) -> dict:
    """Ascending tiers. The first failure is the next tier, then stop."""
    import numpy as np
    attempts = []
    winner = None
    for batch in BATCHES:
        shape = (batch, 1, time_frames, frequency_bins)
        print(f"probe start batch={batch} shape={list(shape)}", flush=True)
        attempt = {"batch": batch, "shape": list(shape), "ok": False}
        failed = False
        tensor = None
        try:
            tensor = torch_module.zeros(
                shape, dtype=torch_module.float32, device="cuda")
            outputs, info = runner.run(tensor)
            if len(outputs) != 3:
                raise RuntimeError("enqueue 返回的 head 数量不是 3。")
            if any(not np.isfinite(head).all() for head in outputs):
                raise RuntimeError("enqueue 完成，但有 head 不是有限数值。")
            attempt["ok"] = True
            attempt["activationBytes"] = info.get("activationBytes")
            attempt["profileIndex"] = info.get("profileIndex")
            winner = batch
            print(f"probe ok batch={batch}", flush=True)
        except Exception as error:
            failed = True
            attempt["error"] = f"{type(error).__name__}: {error}"
            print(f"probe fail batch={batch}: {attempt['error']}", flush=True)
        finally:
            del tensor
            try:
                torch_module.cuda.synchronize()
            except Exception:
                pass
            try:
                torch_module.cuda.empty_cache()
            except Exception:
                pass
        attempts.append(attempt)
        next_tier = None
        if not attempt["ok"]:
            next_tier = {"batch": attempt["batch"], "shape": attempt["shape"],
                         "error": attempt.get("error")}
        report["probe"] = {
            "kind": "runtime-capability",
            "claim": CLAIM,
            "candidates": list(BATCHES),
            "attempts": attempts,
            "maximumRunnableBatch": winner,
            "nextTier": next_tier,
        }
        write_json(report_path, report)
        if failed:
            break
    return report["probe"]


def toolchain_report(args: argparse.Namespace, report: dict) -> None:
    torch_module = prepare_torch(report)
    helpers = import_helpers()
    trt, handles = helpers["import_tensorrt"](args.trt_root)
    version = str(trt.__version__)
    report["tensorRt"] = version
    if version != EXPECTED_TRT:
        raise RuntimeError(
            f"TensorRT 版本是 {version}，本项目运行时要求 {EXPECTED_TRT}。")
    model = load_frozen_model(helpers, args.checkpoint)
    report["checkpointSha256"] = helpers["CHECKPOINT_SHA256"]
    del model
    del handles
    del torch_module
    gc.collect()


def convert(args: argparse.Namespace, report: dict) -> int:
    torch_module = prepare_torch(report)
    helpers = import_helpers()
    trt, handles = helpers["import_tensorrt"](args.trt_root)
    version = str(trt.__version__)
    report["tensorRt"] = version
    if version != EXPECTED_TRT:
        raise RuntimeError(
            f"TensorRT 版本是 {version}，本项目运行时要求 {EXPECTED_TRT}。")
    model = load_frozen_model(helpers, args.checkpoint)
    report["checkpointSha256"] = helpers["CHECKPOINT_SHA256"]
    args.output.mkdir(parents=True, exist_ok=True)
    onnx_path = args.output / "ffscn_epoch1200_tool.onnx"
    print("正在导出 ONNX（复用 model_ffscn，不复制网络）", flush=True)
    onnx = helpers["export_dynamic"](model, onnx_path)
    report["onnx"] = onnx
    del model
    gc.collect()
    profiles = [{
        "index": 0,
        "minShape": [1, 1, args.time_frames, args.frequency_bins],
        "optShape": [1, 1, args.time_frames, args.frequency_bins],
        "maxShape": [16, 1, args.time_frames, args.frequency_bins],
    }]
    report["profile"] = profiles
    write_json(args.report, report)
    precision = args.precision.lower()
    engine_path = args.output / (
        f"ffscn_epoch1200_{precision}_t{args.time_frames}"
        f"_f{args.frequency_bins}_b16.engine")
    print(
        f"开始构建 TensorRT {args.precision} 引擎，"
        f"固定 T={args.time_frames} F={args.frequency_bins}，"
        "profile batch 最大 16",
        flush=True)
    torch_module.cuda.set_device(report["device"]["index"])
    engine = helpers["build_engine"](
        trt, onnx_path, engine_path, profiles, args.workspace_mib,
        precision=args.precision)
    report["built"] = True
    report["engine"] = engine
    report["enginePath"] = str(engine_path.resolve())
    write_json(args.report, report)
    print(f"引擎已写入 {engine_path}", flush=True)
    torch_module.cuda.set_device(report["device"]["index"])
    runner = helpers["DynamicRunner"](trt, engine_path)
    assert_float_io(trt, runner.engine, helpers["OUTPUT_NAMES"])
    probe = probe_batches(
        runner, torch_module, report, args.report, args.time_frames,
        args.frequency_bins)
    manifest = {
        "schema": "signalstudio-ffscn-engine-tool-v1",
        "productionPackage": False,
        "doNotDeployAs": "detection-models/ffscn-gui-iq-v1",
        "precision": args.precision,
        "downNums": 11,
        "checkpointSha256": helpers["CHECKPOINT_SHA256"],
        "engine": {key: engine[key] for key in ("path", "sha256", "bytes")},
        "onnx": {key: onnx[key] for key in ("path", "sha256", "bytes")},
        "input": {
            "name": "spectrum", "dtype": "float32",
            "layout": "contiguous-NCHW",
            "minShape": profiles[0]["minShape"],
            "optShape": profiles[0]["optShape"],
            "maxShape": profiles[0]["maxShape"],
            "timeFrames": args.time_frames,
            "frequencyBins": args.frequency_bins,
            "frequencyPowerOfTwo": True,
            "frequencyMinimum": 2048,
            "downNums": 11,
        },
        "runtime": {
            "tensorRt": version,
            "gpu": EXPECTED_GPU,
            "computeCapability": [12, 0],
            "hardwareCompatibility": "none",
            "versionCompatible": False,
            "torchCuda": report.get("torchCuda"),
        },
        "boundary": CLAIM,
    }
    write_json(args.output / "tool-manifest.json", manifest)
    report["toolManifest"] = "tool-manifest.json"
    report["ok"] = probe.get("maximumRunnableBatch") is not None
    write_json(args.report, report)
    winner = probe.get("maximumRunnableBatch")
    next_tier = probe.get("nextTier")
    print(f"探测结束：最大可运行 batch={winner}，下一档={next_tier}", flush=True)
    del handles
    return 0 if report["ok"] else 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--trt-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--time-frames", type=int, required=True)
    parser.add_argument("--frequency-bins", type=int, required=True)
    parser.add_argument("--precision", choices=("FP32", "FP16"),
                        default="FP32")
    parser.add_argument("--workspace-mib", type=int, default=1024)
    parser.add_argument("--validate-only", action="store_true")
    parser.add_argument("--check-toolchain", action="store_true")
    args = parser.parse_args()
    if args.report is None:
        args.report = args.output / "conversion-report.json"
    return args


def main() -> int:
    args = parse_args()
    report = base_report(args)
    report["mode"] = "validate-only" if args.validate_only else "convert"
    errors = preflight(args)
    if errors:
        report["errors"] = errors
        report["error"] = "\n".join(errors)
        print(report["error"], flush=True)
        if refusal_reason(args.report) is None:
            try:
                write_json(args.report, report)
            except OSError as error:
                print(f"Cannot write report: {error}", file=sys.stderr)
        return 2
    try:
        if args.validate_only:
            if args.check_toolchain:
                toolchain_report(args, report)
            report["validated"] = True
            report["ok"] = True
            report["built"] = False
            write_json(args.report, report)
            print("校验通过，尚未构建引擎。", flush=True)
            return 0
        return convert(args, report)
    except Exception as error:
        report["ok"] = False
        report["error"] = f"{type(error).__name__}: {error}"
        report["traceback"] = traceback.format_exc()
        print(report["traceback"], file=sys.stderr, flush=True)
        if refusal_reason(args.report) is None:
            try:
                write_json(args.report, report)
            except OSError as write_error:
                print(f"Cannot write report: {write_error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
