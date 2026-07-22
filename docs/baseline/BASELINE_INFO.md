# Signal Studio approved document baseline

This directory records the immutable in-repository snapshot used to start the Signal Studio implementation.

| Field | Value |
|---|---|
| Baseline | BL1.0 |
| Document set version | V1.0.0 |
| Approval state | Approved |
| Source supplied to the build | `../Signal_Studio_开发文档/Signal-Studio-Dev-Docs` |
| Resolved source at capture time | `D:\\coding\\signal_studio_prj\\Signal_Studio_开发文档\\Signal-Studio-Dev-Docs` |
| Snapshot directory | `docs/baseline/Signal-Studio-Dev-Docs` |
| Copy time (UTC) | `2026-07-22T07:05:38.022Z` |
| Files | 136 |
| Total bytes | 5,517,356 |
| Digest | SHA-256 |
| Verification | Source and snapshot relative paths, byte lengths, and SHA-256 values all matched |

`baseline-manifest.json` is the machine-readable inventory and verification record. `sha256sums.txt` is the portable checksum list. The snapshot is read-only: implementation decisions must be recorded in the project core documents, not by editing files under `Signal-Studio-Dev-Docs`.

To verify the snapshot from the repository root:

```powershell
python tests/platform/verify_manifests.py baseline
```
