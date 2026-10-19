"""Run clearly separated SCN maximum-spectrum and FFSCN-17 experiments.

These are runtime probes of frozen external model assets, not external business
code. The SCN model sees only maxima of sixteen actual spectra. FFSCN-17 stays
experimental because its PyTorch checkpoint is unavailable and numerical
agreement must be assessed separately for every supported frequency width.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import wave

import numpy as np
import torch

from model_ffscn import sha256_file
from model_probe import frame_spectra, import_tensorrt, tensor_difference


ASSET_HASHES = {
    "scn_model.engine":
        "ae89d04332758d466846b71afc849c446500a82a8bd74fd8c671927247746627",
    "ffscn_17.engine":
        "dd88ab057d5af35e412447428de1a0163f79d0d7ac6a5c1a86308b898d50fd81",
    "ffscn_17.onnx":
        "113d9d64955695790647c26f0d426aa96216f2fc744423a12f21198720fd5867",
}


def stage_asset(source: Path, destination: Path, expected: str) -> None:
    if sha256_file(source) != expected:
        raise ValueError(f"Frozen asset hash differs: {source}")
    shutil.copyfile(source, destination)
    if sha256_file(destination) != expected:
        raise ValueError(f"Staged asset hash differs: {destination}")


class ControlEngine:
    """No shared raw tensor layout assumptions between SCN and FFSCN."""

    def __init__(self, trt, path: Path):
        self.logger = trt.Logger(trt.Logger.WARNING)
        self.runtime = trt.Runtime(self.logger)
        self.engine = self.runtime.deserialize_cuda_engine(path.read_bytes())
        if self.engine is None:
            raise RuntimeError(f"Cannot deserialize {path}")
        self.context = self.engine.create_execution_context()
        self.input_name = self.engine.get_tensor_name(0)
        self.output_names = [self.engine.get_tensor_name(index) for index in
                             range(1, self.engine.num_io_tensors)]
        self.io = [{"name": self.engine.get_tensor_name(index),
                    "shape": list(self.engine.get_tensor_shape(
                        self.engine.get_tensor_name(index))),
                    "dtype": str(self.engine.get_tensor_dtype(
                        self.engine.get_tensor_name(index)))}
                   for index in range(self.engine.num_io_tensors)]

    def __call__(self, value: torch.Tensor) -> list[np.ndarray]:
        if not self.context.set_input_shape(self.input_name, tuple(value.shape)):
            raise ValueError("Input shape not accepted by control engine")
        output_tensors = [torch.empty(
            tuple(self.context.get_tensor_shape(name)), device="cuda",
            dtype=torch.float32) for name in self.output_names]
        self.context.set_tensor_address(self.input_name, value.data_ptr())
        for name, output in zip(self.output_names, output_tensors, strict=True):
            self.context.set_tensor_address(name, output.data_ptr())
        stream = torch.cuda.current_stream()
        if not self.context.execute_async_v3(stream.cuda_stream):
            raise RuntimeError("Control engine enqueue failed")
        stream.synchronize()
        return [output.cpu().numpy() for output in output_tensors]


def resized_frequency(spectra: torch.Tensor, width: int) -> torch.Tensor:
    """The profile's i*srcCount/dstCount interpolation, last sample clamped."""
    coordinates = torch.arange(width, device=spectra.device,
                               dtype=torch.float64) * spectra.shape[-1] / width
    lower = coordinates.floor().to(torch.int64).clamp_max(spectra.shape[-1] - 1)
    upper = (lower + 1).clamp_max(spectra.shape[-1] - 1)
    fraction = (coordinates - lower).to(spectra.dtype)
    result = spectra[:, lower] * (1 - fraction) + spectra[:, upper] * fraction
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--asset-root", type=Path, required=True)
    parser.add_argument("--trt-root", type=Path, required=True)
    parser.add_argument("--wav", type=Path, required=True)
    parser.add_argument("--output", type=Path,
                        default=Path("out/tasks/wideband-detection/M3/assets/controls"))
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for name, identity in ASSET_HASHES.items():
        stage_asset(args.asset_root / name, args.output / name, identity)
    with wave.open(str(args.wav), "rb") as stream:
        if stream.getnchannels() != 2 or stream.getsampwidth() != 2:
            raise ValueError("Control probe requires frozen PCM16 I/Q WAV")
        sample_rate = stream.getframerate()
        raw = stream.readframes(int(sample_rate * 0.02))
    samples = np.frombuffer(raw, dtype="<i2").reshape(-1, 2)
    iq = (samples[:, 0].astype(np.float32) +
          1j * samples[:, 1].astype(np.float32)) / 32768.0
    spectra = frame_spectra(torch.from_numpy(iq).cuda(), sample_rate)
    trt, dll_handles = import_tensorrt(args.trt_root)
    scn = ControlEngine(trt, args.output / "scn_model.engine")
    maximum = spectra[:16].max(dim=0).values
    scn_probes = []
    for start in (0, 16384, 32768):
        window = maximum[start:start + 32768]
        normalized = (window - window.min()) / (
            window.max() - window.min()).clamp_min(torch.finfo(window.dtype).eps)
        value = normalized.reshape(1, 32768, 1, 1).contiguous()
        outputs = scn(value)
        scn_probes.append({"windowStartBin": start,
                           "inputShape": list(value.shape),
                           "finiteOutputs": all(np.isfinite(output).all()
                                                for output in outputs),
                           "outputRanges": [[float(output.min()),
                                             float(output.max())]
                                            for output in outputs]})
    import onnxruntime
    session = onnxruntime.InferenceSession(
        str(args.output / "ffscn_17.onnx"), providers=["CPUExecutionProvider"])
    ffscn = ControlEngine(trt, args.output / "ffscn_17.engine")
    ffscn_probes = []
    for width in (8192, 16384, 32768, 65536, 131072):
        matrix = resized_frequency(spectra[:10], width)
        matrix = (matrix - matrix.mean()) / matrix.std(correction=1).clamp_min(
            torch.finfo(matrix.dtype).eps)
        value = matrix.reshape(1, 1, 10, width).contiguous()
        reference = session.run(["hm", "bw", "off"],
                                {"spectrum": value.cpu().numpy()})
        actual = ffscn(value)
        differences = {name: tensor_difference(first, second)
                       for name, first, second in zip(
                           ("hm", "bw", "off"), reference, actual, strict=True)}
        ffscn_probes.append({"width": width, "inputShape": list(value.shape),
                            "tensors": differences,
                            "passed": all(result["passed"] for result in
                                          differences.values())})
        print(f"FFSCN17 width {width}: {ffscn_probes[-1]['passed']}", flush=True)
    report = {
        "schema": "signalstudio-control-model-probes-v1",
        "gpu": torch.cuda.get_device_name(), "tensorRt": trt.__version__,
        "realWav": str(args.wav.resolve()), "realWavSha256": sha256_file(args.wav),
        "assets": ASSET_HASHES,
        "scn": {"status": "maximum-spectrum-forward-tested-no-accuracy-claim",
                "io": scn.io, "accumulatedFrames": 16,
                "normalization": "per-frequency-window minmax [0,1]",
                "meanSpectrumInferenceCalls": 0,
                "maximumSpectrumInferenceCalls": len(scn_probes),
                "sourceSpectrumUnit": "dBFS/Hz from actual IQ, not calibrated dBm",
                "probes": scn_probes},
        "ffscn17": {"status": "experimental", "io": ffscn.io,
                    "comparison": "Frozen ONNX CPU vs original FP32 engine",
                    "pytorchCheckpointAvailable": False,
                    "probes": ffscn_probes},
        "boundary": "No label accuracy, instrument-domain validation or GUI acceptance",
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    del scn, ffscn
    del dll_handles
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
