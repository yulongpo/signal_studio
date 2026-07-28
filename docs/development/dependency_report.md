# MS-00 依赖报告

`dependencies/dependency-lock.json` 保存不可变已批准输入、获取策略和可移植宿主范围；精确本机检测证据单独保存在 `dependencies/captured-host-evidence.json`。两个校验器均加载只读 BL1.0 依赖来源，并比较每个选定元组：名称、包含 port version 的不可变版本、SPDX、官方来源 URL、vcpkg 锁引用和 baseline 验证值。

## 锁定依赖状态

- vcpkg builtin baseline：`82b6bc886d7b0f8342e34babc2e0b8943f79b0e1`
- 已批准获取脚本归档：`https://github.com/microsoft/vcpkg/archive/82b6bc886d7b0f8342e34babc2e0b8943f79b0e1.tar.gz`
- 归档 SHA-256：`550800632708a561c82412ee69e227c261d0ac8bc381eee09d123014528ae97a`
- 归档大小：5,332,790 字节
- baseline JSON SHA-256：`9f3b13f9a142969a043a921f544c637c54be46b06eabb9025b7e5c28b908af58`
- 选定 ports：qtbase、qttools、intel-mkl、eigen3、tbb、hdf5、nlohmann-json、tomlplusplus、pybind11、onnxruntime、spdlog、fmt、gtest、benchmark

不可变获取脚本、其依赖锁和实时流式下载三者一致：`.tar.gz` 为 5,332,790 字节，SHA-256 为 `550800632708a561c82412ee69e227c261d0ac8bc381eee09d123014528ae97a`。GitHub API 将所请求完整提交解析为相同提交 `82b6bc886d7b0f8342e34babc2e0b8943f79b0e1`，归档根目录为 `vcpkg-82b6bc886d7b0f8342e34babc2e0b8943f79b0e1/`。对照下载的 `.zip` 为 11,349,427 字节，SHA-256 为 `6e63222e536d62a0f26fc6070bce694a3d9e0d42dc0a43d07802500992f08b`；它不是 BL1.0 已批准字节流，依赖锁不接受该表示。

BL1.0 未定义每个 port 的源归档 SHA-256。每个空包哈希因此必须携带 `not-defined-by-bl1.0` 状态，校验器会拒绝没有解释的空值。未虚构任何包哈希。

## Qt 版本语义

不可变 BL1.0 选择 qtbase 6.11.1#1 与 qttools 6.11.1，项目依赖锁逐项保留这些版本；本机检测证据也是 Qt 6.11.1。二者都不自动定义源码最低版本。Visualization/Workbench 的私有 Qt 使用只依赖 Qt 6 通用版本宏；历史远程运行 `29919175820` 已使用 Qt 6.10.3 完成 Windows UI 模块/性能作业，因此项目继续以 6.10.3 为最低支持版本。本轮依赖验证重构的新提交仍需重新运行远程门禁。

`qt_compatibility_contract` 分别记录最低支持 6.10.3、CI 验证 6.10.3、本机验证 6.11.1 和 BL1.0 两个 port 选择。Python/PowerShell 依赖校验器会交叉检查这些字段，既不能篡改不可变 BL1.0 版本，也不能把本机 6.11.1 误当成源码最低版本。

## 三层验证模式

- `Acquisition`：不探测宿主，精确验证不可变 BL1.0 获取、14 个包元组、8 项来源策略和离线缓存元数据；适用于 Ubuntu 与 Windows CI 的便携前置检查。
- `CompatibleHost`：在获取验证之后，要求 Windows x64、匹配工具族和半开版本区间；允许兼容补丁版本和不同安装路径。默认 bootstrap 及 Windows CI 使用此模式，并把当次结果写入生成目录作为证据。
- `ExactCapturedHost`：显式复现审计模式；在兼容检查之外，与已提交快照逐项比较版本、路径、架构、工具族和已安装文件 SHA-256。它不再是其他开发机或 CI 的默认门禁。

注入式模式测试已证明：兼容范围内的新补丁和替代路径可通过，越界版本、错误 Ninja 工具族及 x86 架构被拒绝，精确模式仍拒绝路径变化。当前本机 `Acquisition` 与 `CompatibleHost` 通过；显式 `ExactCapturedHost` 按设计拒绝当前 Windows SDK `rc.exe`，因为版本和路径仍为 10.0.26100.0，但已提交 MS-00 快照哈希为 `43da1503c262c30894c851589bf0155f8365d77e63a5f7bc13982320e3a6b42d`，当前文件哈希为 `65db0d7b4f10ba0f55973fd9356543a556da9ec1c777a0c05f05a0329c8a100a`。这说明通道管理的 SDK 文件在同版本下发生了主机漂移；精确复现审计如实失败，不修改历史快照，也不阻塞默认兼容宿主构建。

## 已安装实例证据与获取契约

Git、CMake、Ninja、Qt/qmake、MSVC/cl、Windows SDK/rc 和 Python 的精确版本、可执行文件路径、已安装文件 SHA-256 与哈希范围保存在独立主机快照中。这些哈希标识本机已测文件，不是厂商安装包哈希，也不参与默认可移植兼容判断。

BL1.0 仅为 vcpkg 归档和可选 CUDA 12.8.1 网络安装器提供获取 URL/SHA-256。Visual Studio/Windows SDK 使用显式 `channel-managed-not-defined-by-bl1.0`；Git、CMake、Ninja、独立 Qt 和 Python 使用 `not-defined-by-bl1.0`。这些状态要求获取 URL/哈希为空并提供策略说明。MS-00 的已提交主机快照记录的是当时未检测到 CUDA；该历史快照不等于 MS-02 的当前检测结论，当前状态见下文“MS-02 最小依赖安装”。

## 离线材料与初始化行为

预期缓存当前缺少已批准 vcpkg `.tar.gz` 与可选 CUDA 安装器。两者都有 BL1.0 已批准 URL 和 SHA-256，且都不是无第三方依赖 MS-00 契约构建的前置条件。材料出现时，初始化会在使用前校验哈希和已知大小。两个校验器会解析不可变获取脚本、展开提交 URL/文件名，并拒绝任何不同的锁或缓存表示。

在同一进程连续执行两次初始化返回相同缺失列表，未修改系统状态，PATH 保持全部条目唯一。最终次数、长度和用户预设字节数以本轮证据日志为准。

## MS-02 最小依赖安装

MS-02 沿用 BL1.0 vcpkg baseline `82b6bc886d7b0f8342e34babc2e0b8943f79b0e1` 和已批准 `.tar.gz` SHA-256。`scripts/install-ms02-dependencies.ps1` 向忽略的 `.deps/vcpkg_installed` 安装以下实际需要的最小集合，二进制缓存位于 `.deps/binary-cache`：

- `intel-mkl:x64-windows@2025.2.0#1`：产品 CPU FFT、卷积和稳定性适配器；
- `libsamplerate:x64-windows@0.2.2#1`：产品成熟重采样适配器；
- `benchmark:x64-windows@1.9.5`：仅用于 Google Benchmark 性能测试。

BL1.0 原有 14 个选定包元组保持原顺序、原字段和原校验值。libsamplerate 作为 MS-02 必要扩展单独记录在可变依赖锁中，锁定版本、BSD-2-Clause、官方来源、同一 vcpkg baseline 和 baseline JSON 校验值；不可变基线文件没有修改。校验器要求“14 个 BL1.0 精确元组 + 显式锁定扩展”，并拒绝未锁定、乱序或字段不一致的附加 port。

安装脚本连续运行必须均报告已安装，且 `VCPKG_MANIFEST_MODE`、`VCPKG_BINARY_SOURCES` 的调用前哨兵值在 `finally` 后原样恢复。

本机已存在 CUDA Toolkit 12.4.131，包含 `cudart64_12.dll`、`cufft64_11.dll`、头文件和导入库。该已安装实例作为用户批准的兼容开发后端使用，不修改不可变 BL1.0 的 12.8.1 获取记录，也不自动执行 NVIDIA 安装程序。cuDNN 不属于 MS-02 的 FFT/内存需求，未安装且不构成阻塞。

生产安装只复制 oneMKL sequential/核心/CPU 与 VML 分派白名单、`samplerate.dll`、CUDA 配置下的 cudart/cuFFT 两个 DLL，以及对应许可证。明确不复制 Google Benchmark、oneMKL SYCL、BLACS、ScaLAPACK、TBB/Intel 线程层、OpenMP、cuFFTW 或开发工具。
