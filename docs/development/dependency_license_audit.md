# 第三方依赖许可证审计

本文记录批准依赖目录以及当前里程碑的分发边界，属于工程证据，不构成法律意见。MS-07 必须依据最终安装包的实际内容重新核对再分发通知、许可证文本和可能的源代码提供义务。

## 批准依赖目录

| 依赖 | SPDX / 许可证 | 当前链接或分发状态 | 工程措施 |
|---|---|---|---|
| Qt Base / Qt Tools | LGPL-3.0-only OR GPL-3.0-only OR Qt Commercial | 本地 UI 测试动态链接 Qt Base；Qt Tools 未链接；当前没有正式发布包 | 保持动态链接和可替换性，随最终制品提供适用许可证、通知和源代码提供说明；商业许可证可替代开源条款 |
| Intel oneMKL 2025.2.0#1 | Intel Simplified Software License | MS-02 动态链接 DFTI、VSL、LAPACKE 的 sequential/lp64 运行时 | 仅携带精确运行时闭包，并安装 `share/SignalStudio/licenses/intel-mkl.txt` |
| libsamplerate 0.2.2#1 | BSD-2-Clause | MS-02 通过私有适配器处理复数 IQ 两通道重采样 | 携带 `samplerate.dll` 和许可证文本，不向公共 API 暴露第三方类型 |
| Eigen | MPL-2.0 | 当前未进入 MS-02 运行时闭包 | 若最终分发，保留适用文件级义务和通知 |
| oneTBB | Apache-2.0 | 当前未进入 MS-02 运行时闭包 | 若最终分发，包含许可证和 NOTICE |
| HDF5 | BSD-3-Clause | 当前未进入 MS-02 运行时闭包 | 若最终分发，包含版权和许可证通知 |
| nlohmann/json | MIT | 当前未进入 MS-02 运行时闭包 | 若最终分发，包含许可证通知 |
| toml++ | MIT | 当前未进入 MS-02 运行时闭包 | 若最终分发，包含许可证通知 |
| pybind11 | BSD-3-Clause | 当前未进入 MS-02 运行时闭包 | 若最终分发，包含许可证通知 |
| ONNX Runtime | MIT | 当前未进入 MS-02 运行时闭包 | 若最终分发，包含许可证并审计执行提供程序的传递二进制 |
| spdlog | MIT | 当前未进入 MS-02 运行时闭包 | 若最终分发，包含许可证通知 |
| fmt | MIT | 当前未进入 MS-02 运行时闭包 | 若最终分发，包含许可证通知 |
| GoogleTest | BSD-3-Clause | 仅用于测试 | 在源代码和依赖材料中保留许可证 |
| Google Benchmark 1.9.5 | Apache-2.0 | 仅用于 MS-02 性能基准，不进入产品运行时闭包 | 在源代码和依赖材料中保留许可证和 NOTICE |
| NVIDIA CUDA Toolkit 12.4.131 / cuFFT | NVIDIA CUDA EULA | 可选后端；本机测试动态链接 `cudart64_12.dll` 与 `cufft64_11.dll` | 不自动安装或接受 EULA；临时安装树仅携带获准再分发的精确运行时和许可证文本 |
| cuDNN | 不适用 | 未使用、未安装、未携带 | 无需操作 |

## MS-00 结论

MS-00 只有本机 Qt 6.11.1 运行时参与测试可执行文件，没有把第三方二进制或许可证文本复制到仓库或最终用户制品。安装消费测试位于忽略的临时构建树，不是发布包；未选择仅 GPL 的 FFT 或图形依赖。

## MS-02 增量结论

MS-02 新增的产品运行时依赖为 Intel oneMKL、libsamplerate，以及启用 GPU 配置时的 CUDA Runtime/cuFFT。它们均位于私有适配器之后，公共头文件不暴露第三方类型。Google Benchmark 只属于测试工具。

安装和洁净 `PATH` 验证必须证明临时安装树仅包含 MSVC 运行时、oneMKL 精确 sequential 分派集合、`samplerate.dll`，以及 CUDA 配置下的 `cudart64_12.dll`、`cufft64_11.dll`；禁止携带 SYCL、BLACS、ScaLAPACK、TBB/OpenMP 线程运行时和开发工具。正式发布仍由 MS-07 按最终制品重新审计。
