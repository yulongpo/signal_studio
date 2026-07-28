# Signal Studio 架构

## MS-00 平台边界

仓库是一个 C++20/C11 CMake 包，包含八个无界面库和两个可选 Qt UI 库。应用层在 MS-04 前不属于实现范围。经批准的依赖 DAG 同时由配置期断言和可执行契约测试机械校验。

| 公共目标 | 直接公共依赖 | 构建类别 |
|---|---|---|
| `SignalStudio::Core` | 无 | 无界面 |
| `SignalStudio::Compute` | Core | 无界面 |
| `SignalStudio::Data` | Core | 无界面 |
| `SignalStudio::TaskRuntime` | Compute、Core | 无界面 |
| `SignalStudio::DSP` | Data、Compute、Core | 无界面 |
| `SignalStudio::ModelRuntime` | Data、Compute、TaskRuntime、Core | 无界面 |
| `SignalStudio::Dataset` | Data、TaskRuntime、Core | 无界面 |
| `SignalStudio::PluginSDK` | Data、TaskRuntime、Core | 无界面 |
| `SignalStudio::Visualization` | Data、TaskRuntime、Core | 可选 UI |
| `SignalStudio::Workbench` | Visualization、TaskRuntime、Core | 可选 UI |

`SIGNAL_STUDIO_BUILD_UI=OFF` 时不调用 `find_package(Qt6)`，也不创建两个 UI 目标。启用 UI 时 Qt Core/Widgets 仍保持 `PRIVATE`。公共头文件检查拒绝 Qt、Eigen、oneMKL、TBB、HDF5、ONNX Runtime、FFTW 以及标准库实现类型。

## 公共契约

- Core 使用中性品牌名 `Signal Processing Platform`。
- `ErrorCode::stable_text()` 生成经批准的 `SS-<DOMAIN>-E###`。单一不变量路径约束十个域、四个原因、原因/类别映射、严重级别、重试/恢复规则，以及最多八层且逐层校验的原因链。工厂拒绝无效值，序列化会再次校验且不能输出 `unknown` Status。`with_context()` 达到八层上限后保留根因与最新上下文、淘汰最旧中间上下文，不因正常的第九次传播抛出异常。
- `CapabilityRegistry` 和版本化 `ModuleDescriptor` 建立发现与模块兼容契约。所有外部可构造枚举先通过显式 `is_known_*` 检查；注册表拒绝未知 `CapabilityAvailability`，描述符拒绝未知自身/依赖 `ModuleId`，空能力列表仍是合法结构。
- `plugin_abi_v1.h` 是 C 兼容边界，包含定宽版本化 POD、64 位不透明句柄、显式调用/导出约定、宿主与插件函数表，以及唯一的 `signal_plugin_query_v1` 入口类型。`SIGNAL_PLUGIN_NOEXCEPT` 在 C++ 中展开为 `noexcept`、在 C 中为空，并覆盖全部回调、查询和校验器签名。C++ 异常边界适配器捕获所有实现异常，并将有返回值回调映射为 `SIGNAL_PLUGIN_RESULT_INTERNAL_FAILURE_V1`；编译期断言和故意抛异常的运行期测试共同约束该契约。C11 示例插件仍可作为 C 编译并导出符号。

数据访问、DSP、调度、渲染、模型和数据集的功能 API 在各自里程碑实现，不在 MS-00 中提前宣称。

## 安装包与本机预设

安装树分别导出 `SignalStudioHeadlessTargets.cmake` 和 `SignalStudioUiTargets.cmake`。`find_package(SignalStudio COMPONENTS Core DSP PluginSDK ...)` 始终只装载无界面导出且不发现 Qt；仅请求 `Visualization` 或 `Workbench` 时才定位 Qt。关闭 UI 构建的安装会如实拒绝 UI 组件请求。

测试分别消费包含全部组件的 UI 安装，以及独立配置、构建、安装并消费的无 Qt 安装；两个消费者分别请求、链接、调用并校验十个和八个可用模块目标。

已提交的预设保持路径中立。初始化/配置发现会写入被忽略的 `CMakeUserPresets.json`：唯一的隐藏 MSVC/Ninja 工具链基预设保存规范化后的完整环境（UI 生成时包含单次 Qt `bin` 与 Qt `CMAKE_PREFIX_PATH`），隐藏 Qt 基预设仅增加短标量 `SIGNAL_STUDIO_QT_ROOT`，各本机别名通过继承复用，避免重复嵌入长环境。所有路径列表按 Windows 大小写不敏感规则规范化去重；Qt/Ninja 受控路径先剔除再单次加入，生成不读取旧用户预设。文件内容确定且通过同目录临时文件原子替换。回归测试在同一进程重复生成三次，并跨 PowerShell 7/Windows PowerShell 5.1 比较，要求字节完全一致、全部路径项唯一且 Qt 根目录仅出现一次。

Windows 路径规范化把盘符根、UNC 根和扩展长度根作为不可再裁剪的路径单位，非根路径才移除尾部分隔符。VS Code 设置、任务、测试和 F5 共用 `local-windows-msvc-debug`；F5 使用明确目标路径，前置校验目标属于同一已配置且非陈旧构建树，构建步骤把目标所需运行库复制到输出目录以支持直接启动。

依赖契约分三层：`Acquisition` 精确验证不可变 BL1.0 获取、14 个包元组、8 项来源策略和离线缓存；`CompatibleHost` 在此基础上按平台、架构、工具族和半开版本区间接受宿主，允许安装路径及兼容补丁版本变化；`ExactCapturedHost` 是显式复现审计，额外比较独立主机快照中的精确版本、路径与已安装文件哈希。默认 bootstrap 和 CI 使用前两层，不把开发机路径误作可移植构建前置条件。

MSVC、Ninja 和 Qt 的进程环境导入保持幂等。MSVC 导入优先复用已有 `VSCMD_VER` 与 `cl.exe` 的 x64 环境；否则先检查 `VSINSTALLDIR`，再通过 `vswhere.exe` 发现带 x64 C++ 工具的 Visual Studio 2022，最后才使用已知本机后备位置。GitHub Windows 2022 的两个 Ninja 作业在 CMake 前显式初始化 x64-hosted x64 环境。

Qt 契约分为三层：项目 UI 源码与安装包的最低支持版本为 Qt 6.10.3，本机实际验证 kit 为 Qt 6.11.1，不可变 BL1.0 的 vcpkg 选择仍为 qtbase 6.11.1#1/qttools 6.11.1。最低版本来自 CI 对当前私有 Qt 使用的真实编译门禁，不把本机版本误写为源码下限。Visualization 与 Workbench 使用相同编译期守卫；CMake、包配置、本机发现脚本、依赖锁扩展字段和静态回归共同拒绝低于 6.10.3 或重新引入 6.11 最低依赖。

远程运行 `29924612586` 已验证质量修复精确提交 `a1c252f873a01fb6ae3a7b0b9e1f60553341b171`：当时的 Ubuntu/Windows 无界面作业与 Qt 6.10.3 Windows UI 模块/性能作业全部通过。MS-00 平台骨架及公共契约已通过最终独立规格与代码质量复审，五项整改验证完成；该里程碑现已关闭。按后续批准的验证策略，MS-01 及以后不再把 Ubuntu 24.04 无界面构建列为架构兼容门禁，CI 仅保留 Windows 2022 无界面和 Windows Qt/UI 作业；历史证据不删除、不改写。后续架构实现已进入 MS-01，本文不据此宣称整体平台或产品完成。

## MS-01 Core、Data 与 TaskRuntime

MS-01 在既有依赖 DAG 内实现三个无界面生产模块，不改变模块的公共依赖方向，也不把 Qt 或第三方实现类型暴露到公共头文件。

### Core

Core 提供统一的 `Result<T>`、服务接口、路径与原子文件能力、SHA-256、配置和日志契约。工程持久化保存完整工作区图、活动对象与自动保存信息；最近工程列表和当前上下文均通过线程安全服务暴露。所有外部输入枚举先验证已知值，失败使用既有结构化 `Status` 返回。

### Data

Data 提供实数/复数样本容器、只读切片、RAW/WAV 描述符解析、分块读取、映射读取、有界预览、选区导出和多分辨率缓存。RAW 支持批准的标量类型、端序和交织/平面布局；WAV 同时校验通道、位深、`blockAlign`、`byteRate`、数据块边界和安全乘法。

长读取的取消结果分为两类：取消前没有形成完整帧时不发布范围；已经形成完整帧时只发布请求起点至最后完整帧的不可变前缀，并把 Data 状态标记为 `partial_read_available`。描述符不一致、文件截断或帧结构损坏仍按解析失败处理，不降级为部分成功。

多分辨率存储的键包含源版本指纹、范围、分辨率和算法参数。内存与磁盘缓存均使用有界 LRU，磁盘项通过锁和原子替换写入；损坏项被诊断并重新生成。预取可取消，频谱概要和 STFT 瓦片来自真实样本计算，限定长度预览不会被标记为全文件结果。

### TaskRuntime

TaskRuntime 是任务生命周期、进度、重试和结构化日志的唯一状态源。它提供有界资源池、优先级队列、依赖 DAG、暂停/恢复/取消、超时、重试、幂等、视图过期、历史和制品恢复。提交与历史恢复统一拒绝零 CPU 单元或零运行时线程，避免绕过资源预算。

崩溃恢复记录只恢复元数据，不伪造可执行工作体。调用重试时必须已有对应 `task_type` 处理器；重试创建新的 TaskId 和 attempt，并按依赖关系递归生成新的依赖任务。缺少处理器时返回明确失败。

Data 的耗时导入通过 TaskRuntime 驱动，但部分数据范围和任务取消状态保持正交：Data 发布可用前缀，TaskRuntime 仍记录 `canceled`。后台任务不依赖 Qt，也不访问 `QWidget`。

### 安装与消费

MS-01 公共 C++ 接口向 MSVC 消费者传递 `/utf-8`，保证中文公共诊断文本不会因消费者源文件代码页而误解析。DebugCRT 与 `ucrtbased.dll` 属于不可再分发调试材料，只复制到本机构建/消费者测试目录，不进入正式安装树；Release 安装树部署匹配的可再分发 VC143 运行库。安装消费者在独立工程中分别验证 UI 全组件包和无 Qt 包，并检查 Debug 安装前缀无调试运行库泄漏、Release 安装前缀运行库完整。UI 消费测试显式把 Qt 6.11.1 `bin` 加入运行环境，包的最低支持版本仍为 Qt 6.10.3。

## MS-02 DSP 与 Compute

Compute 公共层只暴露标准 C++20 类型、能力描述、选择/降级 provenance、内存分配器、预算租约和第三方无关的计算操作契约。生产发现由具体 `IComputeBackend` 实例提供：CPU 标量、SIMD、多线程能力来自宿主查询，CUDA 能力来自 `cudaGetDeviceCount`、设备属性和运行时版本。选择与降级都检查 Working Set 容量。DSP 把 FFT、滤波和重采样实现注册为 Compute 操作；`ComputeRuntime` 统一完成选择、真实执行和失败降级，不再以缓冲区复制冒充算法执行。只有由独立参考结果计算出的最大绝对误差和均方根误差才能标记一致性已验证；操作没有独立参考时会如实记录“未验证”。默认工作线程至少保留一个逻辑核；强制 CUDA 不静默降级，自动模式的失败会明确记录请求后端、实际后端和原因。

DSP 公共层只依赖 Data、Compute 和 Core，不公开 oneMKL、libsamplerate、CUDA 或其他第三方类型。CPU 私有层使用 oneMKL DFTI 及可复用计划缓存实现 FFT，使用 VSL 实现 FIR 卷积，使用 LAPACKE 带状三角求解实现稳定直接型 IIR；复数 IQ 重采样由 libsamplerate 的两通道有状态适配器实现，流结束由显式契约冲刷成熟内核延迟。解析信号通过 DFTI 构造。CUDA 私有层使用 cudart 与 cuFFT 完成分配、H2D、计划、执行、同步和 D2H，版本由 `cudaRuntimeGetVersion`/`cufftGetVersion` 获取。

Compute 与 DSP 是共享模块，第三方链接依赖保持 `PRIVATE`，其余平台模块保持既有 DAG。Windows 运行时闭包只安装 oneMKL sequential/核心/CPU 与 VML 分派 DLL、`samplerate.dll`、CUDA 配置下的 `cudart64_12.dll`、`cufft64_11.dll` 及匹配 VC143 运行库；拒绝 Google Benchmark、SYCL、集群、ScaLAPACK、BLACS、TBB 线程层和 OpenMP 运行时。

处理链采用不可变快照执行。节点校验覆盖类型、Nyquist、IIR 稳定性、抗混叠元数据、重复 ID 和版本；旁路节点在不适用的输入契约校验前返回位一致样本。简单节点循环每 4,096 样本检查取消，第三方内核返回后和最终发布前再次检查，后台线程不接触 `QWidget`。FIR、IIR、重采样状态可跨块延续，模板序列化包含节点契约、系数、比率、模式和参数，旁路区间支持位精确导出。

浏览性能产品路径以真实录制文件为输入，缓存身份由文件大小、修改时间、采样哈希、调用方版本和完整描述符摘要共同组成，避免同路径内容或描述符变化复用旧缓存。路径采用有界视窗读取、内存/磁盘瓦片缓存、采样概览、同一 generation 的时域/PSD/STFT 原子帧，以及 Data 模块的有界 `build_full_sc16_index()` 全文件索引；视窗变化会拒绝旧 generation。MS-02 的大文件证据按用户决定，把指定 4,004,031,888 字节 X310 SC16 录制文件逻辑重复映射为十进制 100 GB，不创建或扫描物理 100 GB 文件；自动化边界覆盖物理尾部拼接、逻辑最后一帧、精确 EOF 和读取上限，结果必须明确记录估计来源。Qt 专项测试只负责实际命令可见绘制、连续交互帧率和三图一致绘制，计算仍在后台产品路径完成；平台插件与测试目标同目录部署，清空三项 Qt 插件环境变量后由自动化启动冒烟验证默认 Windows 插件。
