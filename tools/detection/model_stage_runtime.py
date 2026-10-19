"""Stage official TensorRT headers and complete runtime inventory locally.

Install the fixed official pip package beforehand with:
  python -m pip install --target out/tasks/wideband-detection/M3/assets/runtime/tensorrt-10.11 \
      tensorrt-cu12==10.11.0.33
  python -m pip install --target out/tasks/wideband-detection/M3/assets/runtime/cuda-nvcc-12.9 \
      nvidia-cuda-nvcc-cu12==12.9.86

Header contents come only from NVIDIA's corresponding release branch. No
external application's SDK directory is accepted as a build dependency.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import urllib.request


REPOSITORY_API = "https://api.github.com/repos/NVIDIA/TensorRT"
REF = "release/10.11"
HEADER_COMMIT = "9255eb39e6642787828a4c1f7fc1d09fe004e7a2"


def download_json(url: str):
    request = urllib.request.Request(
        url, headers={"User-Agent": "SignalStudio-model-stage-v1"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response)


def file_identity(path: Path, root: Path) -> dict:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(block)
    return {"path": path.relative_to(root).as_posix(),
            "bytes": path.stat().st_size, "sha256": digest.hexdigest()}


def stage_headers(root: Path) -> dict:
    commit = HEADER_COMMIT
    entries = download_json(
        f"{REPOSITORY_API}/contents/include?ref={commit}")
    include = root / "include"
    include.mkdir(parents=True, exist_ok=True)
    for entry in entries:
        if entry["type"] == "file" and entry["name"].endswith(".h"):
            with urllib.request.urlopen(entry["download_url"], timeout=60) as response:
                (include / entry["name"]).write_bytes(response.read())
    with urllib.request.urlopen(
            f"https://raw.githubusercontent.com/NVIDIA/TensorRT/{commit}/LICENSE",
            timeout=60) as response:
        (root / "LICENSE.TensorRT.txt").write_bytes(response.read())
    version = (include / "NvInferVersion.h").read_text(encoding="utf-8")
    if not re.search(r"#define\s+TRT_MAJOR_ENTERPRISE\s+10\b", version) or \
            not re.search(r"#define\s+TRT_MINOR_ENTERPRISE\s+11\b", version):
        raise ValueError("Downloaded headers are not TensorRT 10.11")
    return {"repository": "https://github.com/NVIDIA/TensorRT",
            "ref": REF, "commit": commit, "includePath": "include"}


def create_import_library(root: Path, msvc_bin: Path) -> str:
    """Create an x64 MSVC import library from the official DLL export table."""
    dll = root / "tensorrt_libs/nvinfer_10.dll"
    dump = subprocess.run([str(msvc_bin / "dumpbin.exe"), "/exports", str(dll)],
                          check=True, capture_output=True, text=True).stdout
    names = []
    for line in dump.splitlines():
        columns = line.split()
        if len(columns) >= 4 and columns[0].isdigit() and \
                all(character in "0123456789ABCDEFabcdef" for character in columns[1]):
            names.append(columns[3])
    if "createInferRuntime_INTERNAL" not in names:
        raise ValueError("TensorRT DLL has no expected runtime creation entry")
    library_dir = root / "lib"
    library_dir.mkdir(parents=True, exist_ok=True)
    definition = library_dir / "nvinfer.def"
    definition.write_text("LIBRARY nvinfer_10.dll\nEXPORTS\n" +
                          "\n".join(names) + "\n", encoding="ascii")
    subprocess.run([str(msvc_bin / "lib.exe"), "/nologo", "/machine:x64",
                    f"/def:{definition}", f"/out:{library_dir / 'nvinfer.lib'}"],
                   check=True, capture_output=True)
    return "Generated with MSVC lib.exe from official nvinfer_10.dll exports"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--msvc-bin", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--cuda-header-root", type=Path, default=Path(
        "out/tasks/wideband-detection/M3/assets/runtime/cuda-nvcc-12.9/nvidia/cuda_nvcc/include"))
    args = parser.parse_args()
    root = args.root.resolve()
    headers = stage_headers(root)
    # CUDA's runtime wheel omits the crt headers required by cuda_runtime_api.h.
    # The matching official nvcc wheel supplies them; no local toolkit fallback.
    shutil.copytree(args.cuda_header_root / "crt",
                    root / "nvidia/cuda_runtime/include/crt", dirs_exist_ok=True)
    import_library = create_import_library(root, args.msvc_bin)
    identities = [file_identity(path, root) for path in sorted(root.rglob("*"))
                  if path.is_file() and path.suffix in (".h", ".dll", ".lib")]
    report = {
        "schema": "signalstudio-tensorrt-runtime-v1",
        "tensorRtVersion": "10.11.0.33", "cudaRuntimeVersion": "12.9.79",
        "configuredRoot": str(root), "portableRoot":
            "out/tasks/wideband-detection/M3/assets/runtime/tensorrt-10.11",
        "pipPackage": "tensorrt-cu12==10.11.0.33",
        "cudaCrtHeadersPackage": "nvidia-cuda-nvcc-cu12==12.9.86",
        "headers": headers, "importLibraryGeneration": import_library,
        "layout": {"include": "include", "importLibrary": "lib/nvinfer.lib",
                   "tensorRtDlls": "tensorrt_libs",
                   "cudaDlls": "nvidia/cuda_runtime/bin",
                   "cudaInclude": "nvidia/cuda_runtime/include",
                   "cudaImportLibrary": "nvidia/cuda_runtime/lib/x64/cudart.lib"},
        "runtimeRequired": ["tensorrt_libs/nvinfer_10.dll",
                            "nvidia/cuda_runtime/bin/cudart64_12.dll"],
        "builderOnly": ["tensorrt_libs/nvinfer_builder_resource_10.dll",
                        "tensorrt_libs/nvonnxparser_10.dll"],
        "pluginPolicy": "Network has no plugin nodes; plugin DLL not needed at runtime",
        "files": identities,
        "boundary": "Official SDK staging; native worker still must be built and tested",
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Staged {len(identities)} SDK/runtime files at {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
