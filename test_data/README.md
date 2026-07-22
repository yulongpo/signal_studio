# Signal Studio test data

This directory contains manifests and small deterministic fixtures. The supplied large recordings remain outside Git in `../test_data` (relative to the repository root) and are never copied into this directory.

## Supplied external recordings

`test-data-manifest.json` records the exact source-relative path, byte length, SHA-256 digest, storage format, and filename/WAV metadata for all four supplied files. The two SC16 recordings contain little-endian interleaved signed 16-bit I/Q pairs. The WAV recording contains two signed 16-bit channels interpreted as I and Q.

Verify every external byte against the manifest:

```powershell
python tests/platform/verify_manifests.py external
```

If the external directory is absent, automated tests return the standard CTest skip code instead of reporting a false pass.

## Minimal deterministic fixtures

Generate or verify the committed small fixtures with Python's standard library:

```powershell
python test_data/generate_minimal_data.py
python test_data/generate_minimal_data.py --check
```

The generator uses fixed parameters and atomically replaces only files inside `test_data/minimal`. It never reads or modifies the supplied external recordings.
