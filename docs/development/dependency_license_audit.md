# MS-00 dependency license audit

This audit records the approved package catalog and current MS-00 distribution boundary. It is engineering evidence, not legal advice. Final redistribution notices and source-offer obligations must be re-audited against the actual MS-07 package contents.

| Dependency | SPDX / license | MS-00 linked or distributed | Engineering action |
|---|---|---|---|
| Qt Base / Qt Tools | LGPL-3.0-only OR GPL-3.0-only OR Qt Commercial | Qt Base linked dynamically for local tests; no release bundle; Qt Tools not linked | Preserve replaceability/dynamic-link compliance, license texts, notices, and applicable source offer; commercial license may supersede |
| Intel oneMKL | Intel Simplified Software License | No | Review runtime redistribution components before MS-07 packaging |
| Eigen | MPL-2.0 | No | Keep modifications/files and notices compliant if packaged |
| oneTBB | Apache-2.0 | No | Include license/notice if distributed |
| HDF5 | BSD-3-Clause | No | Include copyright/license notice if distributed |
| nlohmann/json | MIT | No | Include license notice if distributed |
| toml++ | MIT | No | Include license notice if distributed |
| pybind11 | BSD-3-Clause | No | Include license notice if distributed |
| ONNX Runtime | MIT | No | Include license and audit transitive provider binaries if distributed |
| spdlog | MIT | No | Include license notice if distributed |
| fmt | MIT | No | Include license notice if distributed |
| GoogleTest | BSD-3-Clause | No; test-only pin | Test build only; retain license in source/dependency materials |
| Google Benchmark | Apache-2.0 | No; test-only pin | Test build only; retain license/notice |
| CUDA Toolkit/cuFFT | NVIDIA CUDA EULA | No; toolkit unavailable | Never auto-install; authorized acceptance and redistributable-component review required |

## MS-00 conclusion

Only the existing Qt 6.11.1 runtime participates in local test executables. No third-party binary or license text is copied into the repository or an end-user artifact in this milestone. The installed Signal Studio package test contains the project's static libraries and headers in a temporary ignored build tree; it is not a release bundle. No incompatible GPL-only FFT or graph package is selected.
