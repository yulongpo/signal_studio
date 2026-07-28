# MS-00 工具链报告

检测日期：2026-07-22（Asia/Shanghai）。命令在实际开发主机上通过 `scripts/bootstrap.ps1` 和直接版本查询执行。

| 组件 | 检测结果 | MS-00 用途 |
|---|---|---|
| 操作系统 | Microsoft Windows 11 专业中文版，64 位，10.0.26200 build 26200 | 支持主机 |
| PowerShell | 7.6.4；Windows `powershell.exe -NoProfile` 也已成功执行脚本 | 初始化、配置、构建、测试 |
| CMake | 4.3.1，`D:\softwares\cmake\bin\cmake.exe` | 配置、构建、安装、包导出 |
| Ninja | 1.12.1，`D:\softwares\Qt\Tools\Ninja\ninja.exe` | 构建生成器 |
| Git | 2.53.0.windows.3，`D:\softwares\Git\cmd\git.exe` | 源修订与提交 |
| MSVC | 编译器 19.44.35228；工具集目录 14.44.35207；x64 `cl.exe` 由 VS2022 Build Tools `VsDevCmd.bat` 导入 | C++20 编译 |
| Windows SDK | 10.0.26100.0 | Windows 头文件与运行库导入库 |
| Python | 3.13.12，`D:\profiles\anaconda\python.exe` | 清单与夹具测试 |
| Qt | 6.11.1 MSVC kit；`qmake.exe` 位于 `D:\softwares\Qt\6.11.1\msvc2022_64\bin\qmake.exe`；前缀为 `D:/softwares/Qt/6.11.1/msvc2022_64` | Visualization/Workbench 私有实现依赖 |
| Qt Tools | 同一 Qt kit 内存在 `designer.exe`、`lrelease.exe` 和 `windeployqt.exe` | 后续 UI/部署里程碑 |
| GPU/驱动 | NVIDIA GeForce RTX 5060 Laptop GPU；驱动 591.84；`nvidia-smi` 显示驱动 CUDA 兼容级别 13.1 | 仅检测硬件与驱动 |
| CUDA Toolkit | 无 `nvcc.exe`；CMake 不可用 `CUDAToolkit` | 可选能力不可用；要求并验证 CPU 回退 |

## 发现与获取策略

`SIGNAL_STUDIO_QT_ROOT` 是显式覆盖。UI 预设会扫描主 Qt 安装根下兼容的版本化 `msvc2022_64` kit，设置进程环境，并单次前置所选 kit 的 `bin`。无界面预设不调用 Qt 发现。CMake 与 Ninja 同样自动解析。

初始化/配置生成保持忽略的 `CMakeUserPresets.json`，VS Code 的配置、构建、测试与 F5 统一使用 `local-windows-msvc-debug`。生成器不读取旧用户预设；它从当前 MSVC 环境剔除受控 Ninja/Qt 路径后重新单次加入，唯一隐藏工具链基预设保存一份完整路径环境，Qt 基预设只保存短标量根目录，各别名不再重复长 PATH/`CMAKE_PREFIX_PATH`。PATH、`CMAKE_PREFIX_PATH`、INCLUDE、LIB、LIBPATH 都经过路径规范化与大小写不敏感去重。盘符根、UNC 根和扩展长度根会保留终止分隔符，非根路径才去除尾部分隔符。输出使用 UTF-8 无 BOM、LF、确定顺序、压缩 JSON 与同目录原子替换；该文件保持 Git 忽略。

F5 使用明确的 `signal_studio_platform_tests.exe --case core.version`，前置任务校验本机构建缓存、Ninja 生成器、UI 选项、目标存在性和输入新鲜度；目标构建后复制 Qt 运行库，已在未继承 Qt PATH 的普通 PowerShell 中直接启动通过。

`dependencies/dependency-lock.json` 保存工具族和有界兼容版本，不保存开发机绝对路径；`dependencies/captured-host-evidence.json` 单独保存精确版本、路径和 SHA-256。默认 bootstrap 使用兼容宿主模式，CI 先验证获取契约，再在 Windows 工具链初始化后验证兼容宿主。只有显式精确复现模式读取主机快照。vcpkg/CUDA 携带 BL1.0 URL/SHA-256，没有 BL1.0 可分发物的工具则要求显式来源策略或通道管理状态并保持获取字段为空。

同一 PowerShell 进程重复导入保持幂等：已有 `VSCMD_VER` 与 `cl.exe` 时直接复用；否则按 `VSINSTALLDIR`、`vswhere.exe` 最新 VS2022 C++ 安装、已知本机后备位置的顺序寻找 `VsDevCmd.bat`。后续调用复用标记，MSVC/Ninja/Qt PATH 插入按大小写不敏感规则规范化。Debug→Release 回归与两次初始化的最终 PATH 统计以本轮证据日志为准。

## GitHub Windows 工具链

GitHub `windows-2022` runner 虽安装 Visual Studio，但普通 Ninja 步骤不会自动获得 MSVC/SDK 环境。两个作业先执行不依赖主机路径的 `Acquisition`，两个 Windows 路径再运行仓库脚本：使用 `vswhere.exe` 定位带 `Microsoft.VisualStudio.Component.VC.Tools.x86.x64` 的最新 VS2022 安装，执行 `VsDevCmd.bat -arch=x64 -host_arch=x64`，随后执行 `CompatibleHost`。公共测试脚本检测该已初始化环境后直接复用，不再要求特定 BuildTools 安装目录。

2026-07-26 的最终复核中，`Acquisition` 与 `CompatibleHost` 继续通过；显式 `ExactCapturedHost` 检出 Windows SDK 10.0.26100.0 的 `rc.exe` 字节哈希相对 MS-00 快照发生变化。该模式是按字节复现审计，不是默认构建门禁；历史快照保持不变，漂移已在依赖报告中记录。

Qt 作业使用官方元数据已确认存在的 6.10.3 `win64_msvc2022_64`，再校验 `QT_VERSION`、`VSCMD_ARG_TGT_ARCH`、`VSCMD_ARG_HOST_ARCH`、`QMAKE_XSPEC=win32-msvc` 和 Qt 前缀末级目录。本机实际验证仍使用 6.11.1；项目源码、包配置、本机发现和两个 UI 模块的最低支持版本统一为 6.10.3。远程运行 `29919175820` 是旧实现提交的成功历史证据；本轮质量修复提交需重新执行相同三项作业。

## MS-00 历史配置结果

- `windows-msvc-debug`：使用 MSVC/Ninja、Qt 6.11.1，`CUDA mode: AUTO; toolkit available: OFF`。
- `windows-msvc-release`：使用同一工具链和 CPU 回退。
- `windows-msvc-headless-debug/release`：不依赖 Qt；两个主配置中的嵌套独立无 Qt 配置、构建、安装、消费均通过。
- `windows-msvc-cuda-release`：当时 `CUDA mode: ON; toolkit available: OFF`，旧策略曾警告并继续 CPU 支持；该历史行为已被 MS-02 的显式语义取代。

`nvidia-smi` 的 CUDA 版本是驱动兼容级别，不证明 CUDA Toolkit 已安装。MS-00 不宣称 GPU 构建或测试通过。

## MS-02 工具链更新

2026-07-26 重新检测到 CUDA Toolkit 12.4.131 位于 `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.4`，`cudart`、cuFFT 头文件/导入库/DLL 均完整。RTX 5060 Laptop GPU、驱动 591.84、计算能力 12.0 可由 CUDA Runtime 查询。项目不调用 `nvcc`，而由 MSVC 编译宿主 C++ 后链接 cudart/cuFFT，因此不触发 Toolkit 12.4 对 `sm_120` 设备代码生成的不支持。

oneMKL 2025.2.0#1、libsamplerate 0.2.2#1 和 Google Benchmark 1.9.5 由锁定 vcpkg baseline 安装到项目忽略目录。CMake 对 oneMKL 使用 dynamic/sequential/lp64；libsamplerate 作为 SignalDSP 私有重采样适配器；Google Benchmark 只用于性能测试。Debug/Release 均报告 oneMKL 适配器可用。Qt 继续使用 `D:\softwares\Qt\6.11.1\msvc2022_64`。

CUDA 模式语义在 MS-02 冻结为：`OFF` 禁用 CUDA，`AUTO` 在 Toolkit 不可用时记录原因并显式降级 CPU，`ON` 在 CUDA 12.4.131、cudart 或 cuFFT 缺失时配置阶段硬失败。cuDNN 不在 DSP/Compute 依赖闭包内，因此没有安装。

`CMakeUserPresets.json` 在多次 Debug/Release/CPU/CUDA 配置期间保持单一隐藏公共环境；PATH、INCLUDE、LIB、LIBPATH 和 `CMAKE_PREFIX_PATH` 未递归读取旧文件，也未出现重复条目。显式 CUDA Debug/Release 现有对应测试预设。项目根目录没有遗留 `CMakeFiles/`，所有生成内容位于 `build/` 或 `.deps/`。
