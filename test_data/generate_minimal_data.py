#!/usr/bin/env python3
"""Generate deterministic, dependency-free Signal Studio fixtures."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import struct
import tempfile
import wave


ROOT = Path(__file__).resolve().parent
OUTPUT = ROOT / "minimal"
SAMPLE_RATE = 64_000
FRAME_COUNT = 1_024
TONE_HZ = 8_000
AMPLITUDE = 12_000


def samples() -> list[tuple[int, int]]:
    return [
        (
            round(AMPLITUDE * math.cos(2.0 * math.pi * TONE_HZ * n / SAMPLE_RATE)),
            round(AMPLITUDE * math.sin(2.0 * math.pi * TONE_HZ * n / SAMPLE_RATE)),
        )
        for n in range(FRAME_COUNT)
    ]


def atomic_write(path: Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_name, path)
    except BaseException:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise


def wav_payload(frames: list[tuple[int, int]]) -> bytes:
    descriptor, temporary_name = tempfile.mkstemp(suffix=".wav")
    os.close(descriptor)
    temporary_path = Path(temporary_name)
    try:
        with wave.open(str(temporary_path), "wb") as stream:
            stream.setnchannels(2)
            stream.setsampwidth(2)
            stream.setframerate(SAMPLE_RATE)
            stream.writeframes(b"".join(struct.pack("<hh", i, q) for i, q in frames))
        return temporary_path.read_bytes()
    finally:
        temporary_path.unlink(missing_ok=True)


def digest(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def build_files() -> dict[str, bytes]:
    frames = samples()
    raw = b"".join(struct.pack("<hh", i, q) for i, q in frames)
    return {
        "complex_sine_sc16le.raw": raw,
        "stereo_iq_int16.wav": wav_payload(frames),
        "truncated_sc16le.raw": raw[:7],
    }


def build_manifest(files: dict[str, bytes]) -> bytes:
    metadata = {
        "complex_sine_sc16le.raw": {
            "format": "SC16 raw complex samples",
            "sample_rate_hz": SAMPLE_RATE,
            "tone_hz": TONE_HZ,
            "complex_sample_count": FRAME_COUNT,
            "component_order": "IQ interleaved",
            "byte_order": "little-endian",
        },
        "stereo_iq_int16.wav": {
            "format": "RIFF/WAVE PCM",
            "sample_rate_hz": SAMPLE_RATE,
            "tone_hz": TONE_HZ,
            "frame_count": FRAME_COUNT,
            "channels": 2,
            "channel_semantics": ["I", "Q"],
        },
        "truncated_sc16le.raw": {
            "format": "intentionally malformed SC16",
            "expected_error": "byte count is not divisible by four",
        },
    }
    manifest = {
        "schema": "signal-studio.minimal-test-data/1.0",
        "generator": (PurePosixPath("..") / "generate_minimal_data.py").as_posix(),
        "deterministic": True,
        "files": [
            {
                "name": name,
                "size_bytes": len(payload),
                "sha256": digest(payload),
                "metadata": metadata[name],
            }
            for name, payload in sorted(files.items())
        ],
    }
    return (json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")


def manifest_portability_failures(payload: bytes) -> list[str]:
    failures = []
    if payload.startswith(b"\xef\xbb\xbf"):
        failures.append("manifest contains a UTF-8 BOM")
    if b"\r" in payload or not payload.endswith(b"\n"):
        failures.append("manifest must use LF line endings")
    try:
        document = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        return failures + [f"manifest is not canonical UTF-8 JSON: {error}"]
    canonical = (json.dumps(document, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")
    if payload != canonical:
        failures.append("manifest JSON is not canonically sorted")
    path_values = [document.get("generator", "")]
    path_values.extend(item.get("name", "") for item in document.get("files", []))
    if any("\\" in value for value in path_values):
        failures.append("manifest paths must use POSIX separators")
    names = [item.get("name", "") for item in document.get("files", [])]
    if names != sorted(names):
        failures.append("manifest file entries are not sorted")
    return failures


def check(files: dict[str, bytes], manifest: bytes) -> int:
    expected = {**files, "manifest.json": manifest}
    failures = []
    for name, payload in expected.items():
        path = OUTPUT / name
        if not path.is_file():
            failures.append(f"missing: {path}")
        elif path.read_bytes() != payload:
            failures.append(f"content mismatch: {path}")
    failures.extend(manifest_portability_failures(manifest))
    if failures:
        print("\n".join(failures))
        return 1
    print(f"Verified {len(expected)} deterministic files in {OUTPUT}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="verify without writing")
    args = parser.parse_args()
    files = build_files()
    manifest = build_manifest(files)
    if args.check:
        return check(files, manifest)
    for name, payload in files.items():
        atomic_write(OUTPUT / name, payload)
    atomic_write(OUTPUT / "manifest.json", manifest)
    return check(files, manifest)


if __name__ == "__main__":
    raise SystemExit(main())
