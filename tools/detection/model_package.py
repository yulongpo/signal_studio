"""Stage verified, relocatable model assets beside the standalone worker.

This tool uses Python's standard library only. It neither exports a checkpoint
nor builds an engine; every binary must already match its frozen SHA-256.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil


FROZEN = {
    "ffscn_epoch1200_fp32.engine":
        "d2de250b11d6bc4a3ccec6d95a232b03ba011e085698d525c3454387094bfe81",
    "scn_model.engine":
        "ae89d04332758d466846b71afc849c446500a82a8bd74fd8c671927247746627",
    "nvinfer_10.dll":
        "d100ec93e5abf56f8d2dafb69cde536c4f9fbb4748a5746a74b7009f6894ce67",
    "cudart64_12.dll":
        "760c38928bbe5759f7b31ed6692599eb7ec83cedd5702e84c2b72028a89837e1",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def checked_copy(source: Path, destination: Path, expected: str) -> dict:
    if sha256(source) != expected:
        raise ValueError(f"Frozen asset was modified: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if source.resolve() != destination.resolve():
        if not destination.is_file() or sha256(destination) != expected:
            temporary = destination.with_name(destination.name + ".staging")
            shutil.copyfile(source, temporary)
            if sha256(temporary) != expected:
                raise ValueError(f"Staging copy hash differs: {temporary}")
            temporary.replace(destination)
    if sha256(destination) != expected:
        raise ValueError(f"Packaged asset hash differs: {destination}")
    return {"path": destination.name, "sha256": expected,
            "bytes": destination.stat().st_size}


def model_manifest(profile: str, engine: dict) -> dict:
    scn = profile == "scn-max-psd-v1"
    names = ["Identity_1:0", "Identity:0", "Identity_2:0"] if scn else ["hm", "bw", "off"]
    minimum = [1, 32768, 1, 1] if scn else [1, 1, 10, 65536]
    maximum = [1 if scn else 2, *minimum[1:]]
    output_shape = [0, 8192, 1, 1] if scn else [0, 1, 1, 16384]
    return {
        "schema": "signalstudio-detection-model-package-v1",
        "profileId": profile, "profileVersion": 1, "precision": "FP32",
        "engine": engine,
        "runtime": {"tensorRt": "10.11.0.33", "cudaRuntime": 12090,
                    "gpu": "NVIDIA GeForce RTX 5060 Laptop GPU",
                    "computeCapability": [12, 0],
                    "hardwareCompatibility": "none", "versionCompatible": False},
        "input": {"name": "resnet_32_input:0" if scn else "spectrum",
                  "dtype": "float32", "layout": "contiguous-NCHW",
                  "minShape": minimum, "maxShape": maximum},
        "heads": [{"semantic": semantic, "name": name, "dtype": "float32",
                   "shape": output_shape} for semantic, name in
                  zip(("hm", "bw", "off"), names, strict=True)],
        "boundary": ("SCN frequency control uses maxima only; no accuracy claim" if scn else
                     "FP32 head parity passed; application accuracy requires frozen evaluation"),
    }


def write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes((json.dumps(value, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--asset-root", type=Path, required=True,
                        help="Current M3 assets root, containing epoch1200 and controls")
    parser.add_argument("--runtime-root", type=Path, required=True)
    parser.add_argument("--output-bin", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    models = args.output_bin / "detection-models"
    packaged = []
    for profile, source in (
            ("ffscn-gui-iq-v1", args.asset_root / "epoch1200/ffscn_epoch1200_fp32.engine"),
            ("scn-max-psd-v1", args.asset_root / "controls/scn_model.engine")):
        destination = models / profile / source.name
        entry = checked_copy(source, destination, FROZEN[source.name])
        manifest = model_manifest(profile, entry)
        manifest_path = destination.parent / "manifest.json"
        write_json(manifest_path, manifest)
        packaged.append({"profileId": profile,
                         "manifest": manifest_path.relative_to(args.output_bin).as_posix(),
                         "manifestSha256": sha256(manifest_path), "engine": entry})
    runtime = []
    for relative in ("tensorrt_libs/nvinfer_10.dll",
                     "nvidia/cuda_runtime/bin/cudart64_12.dll"):
        source = args.runtime_root / relative
        runtime.append(checked_copy(source, args.output_bin / source.name, FROZEN[source.name]))
    license_source = args.runtime_root / "LICENSE.TensorRT.txt"
    if not license_source.is_file():
        raise ValueError("Official TensorRT license is missing from the staged SDK")
    license_output = models / "LICENSE.TensorRT.txt"
    checked_copy(license_source, license_output, sha256(license_source))
    cuda_license = args.runtime_root / "nvidia_cuda_runtime_cu12-12.9.79.dist-info/licenses/License.txt"
    if not cuda_license.is_file():
        raise ValueError("Official CUDA runtime license is missing from the staged SDK")
    checked_copy(cuda_license, models / "LICENSE.CUDA.txt", sha256(cuda_license))
    write_json(args.report, {
        "schema": "signalstudio-detection-model-deployment-v1",
        "outputBin": str(args.output_bin.resolve()), "models": packaged,
        "runtimeFiles": runtime,
        "experimental17Packaged": False,
        "dependencyPolicy": "No training repository, Python or SDK path at runtime",
        "verified": True})
    print(f"Verified {len(packaged)} model packages and {len(runtime)} runtime DLLs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
