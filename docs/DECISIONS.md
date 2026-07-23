# Signal Studio 实现决策

## DEV-001 仓库净空替换

- 日期：2026-07-22
- 状态：用户已接受
- 决策：新实现不依赖旧上游项目内容。MS-00 已移除旧原型和旧项目文档，并从用户提供的外部正式文档集独立复制已批准来源。
- 恢复：旧上游 `c205122` 保存在分支 `archive/pre-signal-studio-dev-20260722-145422` 和标签 `pre-signal-studio-dev-20260722-145422`。

## DEV-002 MS-00 依赖模式

- 日期：2026-07-22
- 状态：MS-00 已接受
- 决策：MS-00 使用已安装且兼容的 Qt 6.11.1 与标准 C++20 库。未来完整依赖集由 vcpkg baseline `82b6bc886d7b0f8342e34babc2e0b8943f79b0e1` 锁定；在实际适配器出现前不安装数 GB 的功能依赖。
- 测试影响：MS-00 使用仓库内确定性契约测试可执行文件；GoogleTest/Benchmark 保持锁定，留待后续测试套件使用。

## DEV-003 可选 CUDA 策略

- 日期：2026-07-22
- 状态：符合已批准基线
- 决策：检测 CUDA Toolkit 并在构建信息中公开结果。缺失时，CUDA 预设发出警告并构建 CPU 基线。由于 NVIDIA EULA 需要授权决定，不自动安装 CUDA。

## DEV-004 平台基线采用静态模块包

- 日期：2026-07-22
- 状态：MS-00 已接受
- 决策：MS-00 的十个 C++ 模块库保持静态，插件边界单独定义为版本化 C ABI。SDK 提供 C11 头文件和一个导出 `signal_plugin_query_v1` 的共享插件示例，兼容性由 C/C++ 消费者和运行期符号查找共同校验。

## DEV-005 无界面与 UI 包组件

- 日期：2026-07-22
- 状态：MS-00 实现复审已接受
- 决策：Core、Data、DSP、Compute、TaskRuntime、PluginSDK、ModelRuntime 和 Dataset 可在无 Qt 情况下配置、构建、安装和消费。Visualization 与 Workbench 为可选 UI 组件。安装包仅在请求 UI 组件时发现 Qt。

## DEV-006 依赖获取证据

- 日期：2026-07-22
- 状态：MS-00 实现复审已接受
- 决策：保留已批准 vcpkg baseline，并由两个校验器将全部选定包元组与不可变 BL1.0 依赖锁逐项比较。已安装宿主工具以版本、路径和可执行文件哈希记录本机证据，与获取契约分离。仅对 BL1.0 明确定义的 vcpkg 和可选 CUDA 断言获取 URL/SHA-256；其他来源策略或通道管理缺失必须显式编码。缓存材料缺失会如实报告，但不阻断无第三方依赖的 MS-00 构建。

## DEV-007 C ABI 异常边界

- 日期：2026-07-22
- 状态：MS-00 实现复审已接受
- 决策：每个 C ABI 回调、查询和校验器都具有可移植无抛出签名。C++ 插件实现通过全捕获适配器把有返回值回调异常转换为稳定的 `SIGNAL_PLUGIN_RESULT_INTERNAL_FAILURE_V1`；无返回值回调因没有结果通道而仅执行异常封闭。故意抛异常的运行期探针和编译期 `nothrow` 断言均为强制兼容检查。

## DEV-008 结构化 Status 不变量

- 日期：2026-07-22
- 状态：MS-00 实现复审已接受
- 决策：单一内部校验器定义合法的 BL1.0 域/原因、稳定原因/类别对应、严重级别、重试/恢复约束，以及原因链类别、代码与深度语义。失败工厂拒绝无效结构；序列化重新校验，不生成兜底 `unknown` 值。

## DEV-009 精确 BL1.0 vcpkg 归档格式

- 日期：2026-07-22
- 状态：符合已批准基线
- 决策：vcpkg 提交 `82b6bc886d7b0f8342e34babc2e0b8943f79b0e1` 仅通过不可变 BL1.0 获取脚本中以 `.tar.gz` 结尾的 URL 获取。实时字节流校验得到 5,332,790 字节和 SHA-256 `550800632708a561c82412ee69e227c261d0ac8bc381eee09d123014528ae97a`，与 BL1.0 完全一致。同一提交的 `.zip` 表示为 11,349,427 字节，SHA-256 为 `6e63222e536d62a0f26fc6070bce694a3d9e0d42dc0a43d07802500992f08b`，不能与经批准归档互换。
- 安全与可复现影响：两个校验器均解析不可变获取脚本，并强制项目锁中的提交、展开后 URL、SHA-256 和归档文件名完全一致。即使 URL 指向同一提交，也拒绝替换归档格式。

## DEV-010 GitHub Windows Ninja 工具链初始化

- 日期：2026-07-22
- 状态：本地修复与第三轮远程三项 CI 门禁通过；独立规范复审待再次核查
- 决策：两个 Windows 2022 Ninja 路径均在 CMake 前导入 `VsDevCmd.bat -arch=x64 -host_arch=x64`。公共脚本先复用已存在的 `VSCMD_VER` 与 `cl.exe`，否则按 `VSINSTALLDIR`、`vswhere.exe` 最新 VS2022 C++ 安装、已知本机后备位置的顺序发现环境。CI 全局固定 Python UTF-8，第三方 Action 使用完整提交哈希。
- Qt 决策：项目源码、Visualization/Workbench 编译期守卫、本机发现、CMake 与安装包的最低支持版本统一为 Qt 6.10.3。本机实际验证 kit 继续为 6.11.1；不可变 BL1.0 的 qtbase 6.11.1#1/qttools 6.11.1 是依赖选择，不是源码最低版本。依赖锁用独立兼容性字段明确区分这些语义。
- 原因：仅选择 Ninja 不会初始化 MSVC 环境；Python 传统代码页不能安全打印中文路径；Qt 6.11.1 的既有 Action 获取地址返回 404。远程运行 `29918020386` 暴露遗留 `QT_VERSION_CHECK(6, 11, 0)` 与声明最低版本的矛盾；修复后，运行 `29919175820` 已在提交 `d41c2748a465c4e843617e0a9444c8f8cc2f5015` 上验证 Qt 6.10.3 Windows UI 模块/性能作业及两项无界面作业全部通过。当前私有 Qt 实现只使用 Qt 6 通用的 `qglobal.h` 版本宏，故将经过实际 CI 编译门禁的 6.10.3 作为支持下限，并由静态回归防止再次提升为本机版本。

## DEV-011 本机 CMake 用户预设生成

- 日期：2026-07-22
- 状态：MS-00 已接受
- 决策：机器相关的 `CMakeUserPresets.json` 继续由 Git 忽略。生成器以唯一隐藏工具链基预设共享一份完整 MSVC/Ninja/可选 Qt 路径环境，单独 Qt 基预设仅保存短标量根目录；不从旧生成文件回读环境。PATH、`CMAKE_PREFIX_PATH`、INCLUDE、LIB 和 LIBPATH 在写入前进行路径规范化及 Windows 大小写不敏感去重。
- 一致性影响：输出采用稳定顺序、UTF-8 无 BOM 和 LF，并通过同目录临时文件原子替换。回归测试在同一进程重复生成，要求字节完全一致、路径项唯一、Qt 根目录单一，防止递归膨胀。

## DEV-012 依赖契约分层验证

- 日期：2026-07-22
- 状态：已验证并关闭；精确提交远程 CI 与最终独立复审通过
- 决策：依赖锁拆分为不可变获取/包元组、宿主兼容范围和独立精确主机快照。`Acquisition` 在各平台精确验证 BL1.0 获取、14 个包元组、8 项来源策略和离线缓存；`CompatibleHost` 额外验证 Windows x64、工具族及半开版本区间，允许兼容补丁和安装路径变化；`ExactCapturedHost` 仅供显式复现审计，比较快照中的版本、路径和已安装文件哈希。
- 历史 CI 影响：MS-00 验收时 Ubuntu 与 Windows 都运行 `Acquisition`，Windows 初始化 MSVC/Qt 后运行 `CompatibleHost`。默认 bootstrap 不再要求开发机精确路径，现有精确证据仍保留在 `dependencies/captured-host-evidence.json`。MS-01 起的 CI 平台范围由 DEV-014 更新，依赖契约分层本身不变。

## DEV-013 工具链与公共契约稳健性

- 日期：2026-07-22
- 状态：已验证并关闭；精确提交远程 CI 与最终独立复审通过
- 决策：VS Code 设置、任务、测试与 F5 统一使用本机 Debug 预设及构建树，F5 采用明确平台测试目标并校验缓存、生成器、选项、目标和输入新鲜度；Windows 路径规范化保留盘符、UNC 与扩展长度根；所有外部可构造公共枚举先做已知值检查。
- Status 边界：八层原因链已满时继续添加上下文不会抛异常；保留根因和最新上下文，并淘汰最旧中间上下文，使传播 API 在设计容量边界内保持可用。
- 收口：依据最终独立复审、本地双配置测试、VS Code 工作流验证及精确提交远程 CI 结果，MS-00 判定为验收通过并关闭。历史失败证据继续保留用于审计，但不再代表当前状态；下一里程碑为 MS-01。

## DEV-014 停止 Ubuntu 24.04 无界面构建门禁

- 日期：2026-07-22
- 状态：用户批准，自 MS-01 起生效
- 决策：持续集成仅保留 Windows 2022 无界面构建/测试和 Windows 2022 Qt/UI 构建/测试，不再执行 Ubuntu 24.04 无界面构建测试。
- 影响：工作流和静态校验器拒绝重新引入 Ubuntu 作业；后续里程碑不得把未执行的 Linux 构建写为验收通过，也不得据此新增 Linux 兼容声明。MS-00 既有 Ubuntu 运行及其文档保持原样，作为当时提交的历史证据。

## DEV-015 MS-01 收口与本机 CUDA 可用性

- 日期：2026-07-23
- 状态：MS-01 已验收关闭
- 决策：MS-01 在 MS-00 平台骨架上交付 SignalCore/SignalData/SignalTaskRuntime 生产契约（54 项批准需求）。验收以无界面 Debug/Release 与 UI Debug/Release 四个配置全量 CTest 通过（414/414）为据，不引入第三方依赖，依赖 DAG 不变。
- 环境变化：当前开发主机已安装 CUDA 12.4（`nvcc` 可用），与 MS-00 记录的“无 nvcc/CUDA Toolkit”不同。GPU 数值验证自 MS-02 起启用；MS-01 不报告 GPU 结果。
- 构建修复：`scripts/common.ps1` 的 `Update-SignalStudioUserPresets` 现在捕获 `VCToolsRedistDir`/`UniversalCRTSdkDir`/`UCRTVersion`，使裸 `cmake --preset local-*` 在脚本进程外也能部署 VC143 运行库并查找 Debug UCRT。脚本流程（`configure.ps1`/`build.ps1`）通过 `Import-SignalStudioMsvcEnvironment` 提供完整 MSVC 环境，仍为 CI 与 VS Code 的正式入口。
- 下一里程碑：MS-02（DSP 与 Compute 后端），采用自包含 CPU FFT/PSD/STFT/窗函数/滤波/重采样实现作为默认后端，cuFFT 作为可选 GPU 适配器；oneMKL/FFTW 因本机未安装而作为环境偏差如实记录，经适配器接口保留后续替换能力。

## DEV-016 MS-02 DSP/Compute 后端与 oneMKL 环境偏差

- 日期：2026-07-23
- 状态：MS-02 已验收关闭
- 决策：SignalCompute 经 `IComputeBackend`/`IBackendSelector` 实现 CPU/SIMD/CUDA 能力探测与自动选择/显式降级（ADR-009）。SignalDSP 经 `IFftBackend` 适配器隔离 FFT 后端（ADR-006）：cuFFT Z2Z 为 GPU 后端（CUDA 12.4 可用），oneMKL 为 CPU 默认后端。窗函数/统计/滤波（FIR 窗 sinc）/重采样（多相）为标准成熟 DSP 实现（非 FFT/矩阵，不违反 ADR-006 的自研 FFT 禁令）。
- 环境偏差：本机未安装 oneMKL；vcpkg manifest 模式会源码编译 Qt（与系统 Qt 6.11.1 冲突），单独 `intel-mkl` 端口响应极慢未完成。故 CPU FFT 适配器接口就绪但实现返回 `unavailable`，CPU 构建不注册 FFT/PSD/STFT 用例。cuFFT GPU 后端经解析信号（单音 bin、Parseval、IFFT 往返、PSD 频率、STFT 时频）验证。
- 影响：公共头无第三方类型；cuFFT/cudart PRIVATE 链接，依赖 DAG 不变；`SignalStudioConfig.cmake` 在 CUDA 构建时 `find_dependency(CUDAToolkit)`，安装包消费者可解析 CUDA 目标。oneMKL 装好后经 `SIGNAL_STUDIO_USE_MKL` 选项接入，无需改动公共 API。
- 下一里程碑：MS-03（Visualization 与 Workbench），DSP 结果驱动 Qt 图表。
