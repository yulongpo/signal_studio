# Deterministic Import Fixtures

The two supplied A1.2 files are byte-for-byte copies used by native UI tests:
`IQ0_FS1Msps_BW800kHz_FC100MHz.dat` and `real_ADC_FS1Msps_FC0Hz_RI16.raw`.
Each has 16,384 samples per channel. The first RAW file has 65,536 bytes;
the second has 32,768 bytes. They are not generated preview data.

`raw_fixture.h` independently writes six encodings, both endian orders,
IQ/QI/Planar and channel-interleaved/planar data for the 432-case reader/v4
matrix and 72-case DDC matrix. Tests also write deterministic coherent tones,
integer extrema and non-finite floating values, and check old CI16 equality.

An additional independent Python standard-library generator uses seed
`12002026`. It writes six small fixtures, exact reference samples, file hashes,
seven header bytes, five trailer bytes, and a deliberately truncated file.
Its destination must not exist; it never overwrites existing sample files.

```powershell
python tests/fixtures/generate_import_fixtures.py artifacts/import-fixtures-m5
out/vs2026-qt611-release_bin/SignalStudioSampleFormatTests.exe artifacts/import-fixtures-m5
scripts/measure-import-load.ps1 -Configuration Release
```

The optional manifest argument verifies all 384 reference samples and SHA-256
before rejecting the truncated fixture. Normal CTest runs the same C++ matrix
without requiring Python. Generated fixtures are temporary, not build inputs.
The load measurement creates and deletes a 64 MiB zero-code CI16 RAW file;
it performs actual bounded reads/decoding and samples Windows PeakWorkingSet64.
This single local measurement is not a general throughput guarantee.
