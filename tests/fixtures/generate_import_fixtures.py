"""Small deterministic RAW fixtures plus independent expected sample values.

Usage: python tests/fixtures/generate_import_fixtures.py OUTPUT_DIRECTORY
Then: SignalStudioSampleFormatTests.exe OUTPUT_DIRECTORY
The output must not exist; this script never overwrites user samples.
"""
import argparse
import hashlib
import json
import random
import struct
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    rng = random.Random(12002026)
    values = [[complex(rng.randint(-100, 100), rng.randint(-100, 100))
               for _ in range(64)] for _ in range(3)]
    records = []
    for name, encoding, order, iq, channels, layout, selected in [
        ("cf32", 4, 1, 1, 1, 0, 0),
        ("ri16-be", 2, 2, 0, 1, 0, 0),
        ("iq", 2, 1, 1, 1, 0, 0),
        ("qi", 2, 2, 2, 1, 0, 0),
        ("planar", 2, 1, 3, 3, 0, 2),
        ("channel-planar", 2, 2, 3, 3, 1, 1),
    ]:
        real = iq == 0
        fmt = {"structure": "real" if real else "complex",
               "encoding": ["I8", "U8", "I16", "I32", "F32", "F64"][encoding],
               "byteOrder": "be" if order == 2 else "le",
               "iqLayout": ["na", "iq", "qi", "planar"][iq],
               "channels": channels, "channelLayout": "planar" if layout else "interleaved",
               "selectedChannel": selected, "headerBytes": "7", "trailerBytes": "5",
               "normalizeIntegerAdc": False}
        components = []
        def sample(c, n, q):
            components.append(values[c][n].imag if q else values[c][n].real)
        if iq == 3:
            if layout:
                for c in range(channels):
                    for q in (False, True):
                        for n in range(64):
                            sample(c, n, q)
            else:
                for q in (False, True):
                    for n in range(64):
                        for c in range(channels):
                            sample(c, n, q)
        else:
            for n in range(64):
                for c in range(channels):
                    for q in ((False,) if real else (True, False) if iq == 2 else (False, True)):
                        sample(c, n, q)
        code = (">" if order == 2 else "<") + ("f" if encoding == 4 else "h")
        payload = b"H" * 7 + b"".join(struct.pack(code, v if encoding == 4 else int(v)) for v in components) + b"T" * 5
        filename = name + ".raw"
        (args.output / filename).write_bytes(payload)
        records.append({"file": filename, "sampleFormat": fmt,
                        "expected": [[z.real, 0 if real else z.imag] for z in values[selected]],
                        "sha256": hashlib.sha256(payload).hexdigest()})
    (args.output / "truncated.raw").write_bytes(payload[:-1])
    (args.output / "manifest.json").write_text(json.dumps({"seed": 12002026, "fixtures": records}, indent=2), encoding="ascii")
    print(f"Generated {len(records)} valid fixtures and one truncated fixture in {args.output}")


if __name__ == "__main__":
    main()
