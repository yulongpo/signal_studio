# Signal Studio 测试计划

## MS-00 测试矩阵

Debug 与 Release UI 配置各执行 45 个具名 CTest 用例。无 Qt 包测试会在移除 Qt 发现变量后，执行一套嵌套且独立的配置、构建、安装和消费流程。

| 覆盖范围 | 每个配置的具名用例数 |
|---|---:|
| 各模块单元/契约/兼容检查 | 10 |
| Core 版本、错误不变量、枚举边界、模块描述符与 DAG 契约 | 8 |
| 插件 ABI：C 编译/链接、C++ `noexcept`/布局、异常封闭、导出符号/版本拒绝 | 4 |
| 通用确定性非发布基准冒烟 | 1 |
| 十个模块的独立确定性性能冒烟 | 10 |
| 基线、外部/最小数据、公共 API、依赖锁、本机配置 | 6 |
| VS Code 本机预设、任务、F5 构建树一致性 | 1 |
| 同进程 PowerShell 环境与用户预设幂等性 | 1 |
| Windows 盘符/UNC/扩展长度根与去重语义 | 1 |
| 获取、兼容宿主、精确快照三种依赖验证模式 | 1 |
| 已安装包消费者：全部组件和无 Qt 无界面组件 | 2 |
| 合计 | 45 |

十个模块用例分别校验描述符结构、API 版本、目标命名空间和能力契约。每个模块另有独立注册的性能测试，通过该模块实现提供器执行 100,000 次真实调用，并周期性校验契约；非发布保护阈值宽松设为五秒。通用基准另执行 250,000 次能力查找。这些测试属于确定性冒烟/回归保护，不是发布性能结论。精确 DAG 测试覆盖跨模块兼容，安装消费者调用全部可用模块目标。

公共 API 检查扫描全部 `.h` 与 `.hpp`，拒绝 Qt、Eigen、oneMKL、TBB、HDF5、ONNX Runtime、FFTW 以及标准库实现类型。枚举负向测试覆盖未知能力可用性、未知模块及未知依赖模块；Status 传播覆盖第七、八、九次上下文添加，验证上限不抛出、根因保留和最新上下文可见。已提交的预设和 VS Code JSON 会扫描本机绝对路径。PowerShell 环境回归还会在同一进程三次生成被忽略的 `CMakeUserPresets.json`，要求字节完全稳定、文件小于 32 KiB、预设名唯一、PATH/`CMAKE_PREFIX_PATH`/INCLUDE/LIB/LIBPATH 项按大小写不敏感规则唯一，并确保 Qt 根目录只出现一次。

Python 与 PowerShell 依赖校验器逐项比较 BL1.0 的选定名称、版本、SPDX、来源、锁和验证字段，并强制未定义哈希具有显式策略。两个校验器还解析不可变 BL1.0 获取脚本，要求 vcpkg 提交、展开后的 `.tar.gz` URL、SHA-256、大小和离线缓存文件名完全一致。模式回归使用注入清单证明：兼容补丁和不同安装路径可接受，不兼容版本、工具族或架构被拒绝，精确快照模式仍拒绝路径变化。Python 清单用例固定 `PYTHONUTF8=1` 与 `PYTHONIOENCODING=utf-8`，校验器还会把标准输出/错误流重配置为 UTF-8 并使用安全替换策略，保证 Windows 传统代码页下的中文缺失路径不会触发编码异常。

最小数据生成器以排序 JSON、POSIX 路径、UTF-8 无 BOM 和 LF 生成清单；`.gitattributes` 固定 Windows 检出仍为 LF。`--check` 同时检查规范序列化、路径分隔符、文件排序和字节一致性，清单验证还检查 Git 属性，防止只在 Windows 出现的 CRLF 差异。

GitHub Actions 工作流仅在 Windows 2022 执行两项门禁：无界面平台/C SDK 示例，以及 Qt/UI 全模块。两个 Windows Ninja 配置前均显式初始化或复用 x64-hosted x64 MSVC 环境。Qt Windows 作业安装并校验最低支持版本 Qt 6.10.3 `win64_msvc2022_64`，构建十个模块并运行十个性能冒烟用例；本机验证使用 Qt 6.11.1，不可变 BL1.0 port 版本仍保持原值。所有第三方 Action 固定完整提交哈希。自 MS-01 起不再执行 Ubuntu 24.04 无界面构建测试，也不据此声明新的 Linux 兼容性；MS-00 已完成的 Ubuntu 运行仅作为历史证据保留。

`scripts/validate-ci-workflow.py` 强制 Windows 2022 单平台矩阵、UTF-8 环境、初始化顺序、`Acquisition`/`CompatibleHost` 步骤顺序、Action 固定、Qt 版本/ABI 与官方可用性证据契约，并拒绝重新引入 Ubuntu 作业。`scripts/validate-vscode-workflow.py` 静态验证本机预设、任务链和明确 F5 目标，并可在构建后动态验证同一构建树及目标新鲜度。既有 `verification.portable_config` 同时扫描 CMake、安装包模板、本机 Qt 发现脚本、Visualization 与 Workbench 编译期守卫：全部必须使用 6.10.3，并拒绝把本机 6.11 身份重新写成最低版本。质量修复精确提交曾由远程运行 `29924612586` 验证 Windows UI、Windows 无界面和 Ubuntu 无界面作业全部成功；该记录属于 MS-00 历史证据。最终独立规格与代码质量复审均通过。

CUDA 保持可选。缺失 Toolkit 时必须报告并保持 CPU 可构建；没有 `nvcc` 和真实后端时，不宣称 GPU 数值或性能通过。
MS-00 最终验证已收口：本地 Debug 45/45、Release 45/45，合计 90/90；VS Code 任务树 45/45 与直接 F5 目标通过；GitHub Actions 运行 `29924612586` 的三个精确提交作业通过。MS-00 已关闭，后续测试活动从 MS-01 继续。

## MS-01 测试矩阵

MS-01 的 54 项具名需求测试由 Core 11 项、Data 32 项和 TaskRuntime 11 项组成：

| 范围 | 需求 | 每个配置用例数 |
|---|---|---:|
| 工程、上下文与 Core 平台 | `FR-PRJ-001`～`FR-PRJ-010`、`FR-CORE-101` | 11 |
| 数据、索引与 Data 平台 | `FR-DAT-001`～`FR-DAT-019`、`FR-IDX-001`～`FR-IDX-012`、`FR-DATA-101` | 32 |
| 任务调度与 TaskRuntime 平台 | `FR-TSK-001`～`FR-TSK-010`、`FR-TASK-101` | 11 |
| 合计 |  | 54 |

每个测试使用独立临时根目录；异步观察使用最长五秒的有界谓词等待，允许 `ctest --parallel 8` 执行而不共享夹具状态。数据测试覆盖真实 RAW、WAV、外部 WAV 和 SC16 窗口读取、描述符负向校验、部分读取、选区导出、缓存损坏恢复和预取取消。任务测试覆盖资源预算、优先级、依赖 DAG、暂停/恢复/取消、超时、重试、幂等、过期、结构化失败、历史和崩溃恢复。

重点负向边界包括：

- 零完整帧取消不发布数据；有完整帧取消只发布完整前缀，任务保持取消；
- WAV 的 `blockAlign`、`byteRate`、乘法溢出和截断分别被拒绝；
- 零 CPU 单元或零运行时线程在提交、历史追加和恢复阶段都被拒绝；
- 恢复任务缺少处理器时不能重试；注册处理器后使用新 TaskId 和新 attempt 真实执行；
- 缓存键形状、未知枚举、非有限或回退进度均被拒绝。

### 本地候选验证结果

| 预设 | 配置 | 全量 CTest | 结果 |
|---|---|---:|---|
| `windows-msvc-debug` | UI、Debug | 106/106 | 通过 |
| `windows-msvc-release` | UI、Release | 106/106 | 通过 |
| `windows-msvc-headless-debug` | 无 Qt、Debug | 101/101 | 通过 |
| `windows-msvc-headless-release` | 无 Qt、Release | 101/101 | 通过 |
| `windows-msvc-cpu-debug` | 强制 CPU、Debug | 106/106 | 通过 |
| `windows-msvc-cpu-release` | 强制 CPU、Release | 106/106 | 通过 |

Debug 和 Release 的 54 项精确需求集合分别为 54/54；UI 与无 Qt 安装树消费者在两种配置下均为 2/2。22 个变更 C/C++ 文件通过 `clang-format --dry-run --Werror`。8 个生产 `.cpp` 已执行 `clang-tidy`，均退出码 0 且为 0 errors；共记录 123 次 warning occurrence，其中包含共享头和系统实现的重复诊断。独立代码质量复审已审阅真实类别并完成七项 Important 整改，最终复审无剩余 Critical/Important，但不把工具执行成功表述为零告警。`git diff --check`、公共头隔离、依赖 DAG 和基线完整性检查通过。

CUDA 保持可选且未自动安装；强制 CPU 预设证明缺少 GPU 后端不影响构建。按照 DEV-014，本里程碑未执行 Ubuntu 24.04 无界面构建，也不新增 Linux 兼容声明。实现树由提交 `39f1d0f2ae9b2cc063543cbdbe69bc3ddd388fd2` 固定，并通过远程整合提交 `c89412e615168b067f3f29646e778b6de5c8b1b5` 的 Windows GitHub Actions 运行 `30187026089` 验证：`headless-build-test` 与 `windows-ui-module-performance` 均成功。MS-01 测试活动已收口。

## MS-02 测试矩阵

MS-02 有 30 项逐需求单元/契约测试：`FR-DSP-001`～`FR-DSP-012`、`NFR-NUM-001`～`NFR-NUM-005`、`NFR-PERF-001`～`NFR-PERF-011`、`FR-DSP-101` 和 `FR-COMPUTE-101`。另有 3 项 Qt 实际 `paintEvent` 专项、1 项清空插件环境变量后的默认 Windows 平台启动回归、1 项 Data→DSP→Cache 外部数据集成测试和 1 项独立 Google Benchmark，共 36 个 `ms-02` 标签用例；其中 30 项逐需求测试保持唯一需求归属，Qt 专项是性能需求的附加实绘证据。公共头隔离验证是矩阵之外的门禁。

数值测试使用 oneMKL、libsamplerate 和可选 cuFFT 生产适配器，覆盖 FFT 正逆变换、PSD/ENBW、STFT、频率轴、FIR、LAPACKE 带状三角 IIR、高阶 IIR 跨块 golden、重采样分块等价与抗混叠、NaN/Inf、零信号、动态范围和样本边界。CUDA 配置真实执行设备探测、上下文切换与恢复、cudaMalloc、H2D、cuFFT、同步、D2H、OOM 恢复和绑定设备的内存分配器；单 GPU 主机只把多设备切换基准记录为环境跳过，不虚构第二块设备。

性能测试不得用空循环、睡眠或测试内自建的替代算法冒充生产路径：

- UI 命令反馈与连续交互同时验证生产 Compute/DSP 路径和 Qt 实际绘制；
- 热替换和首屏恢复使用真实内存/磁盘瓦片缓存；
- 采样概览、三图冷结果和热恢复使用 `LogicalRecordingSource`、`BrowsePerformanceSession` 及真实 Spectrum/PSD/STFT；
- `NFR-PERF-009` 对用户指定的 4,004,031,888 字节 X310 SC16 文件调用生产 `build_full_sc16_index()`，以 64 MiB 有界块覆盖 1,001,007,972 帧和 955 个索引 bin；Release 先预热，再做 3 轮成对交替的同盘单流顺序读取/完整索引比较。顺序读取只抽样每块首尾字节，不把逐字节校验和 CPU 开销混入基线；Debug 只做一次全文件结构验证；
- `NFR-PERF-004` 覆盖物理文件尾重复拼接、逻辑最后一帧、精确 100,000,000,000 字节 EOF 及显式读取上限；`NFR-PERF-006` 在 8 GB 与 100 GB 逻辑空间各执行 48 个远距离三图视窗并逐轮采样峰值 Working Set；`NFR-PERF-010` 使用不同冷热视窗并验证旧代际拒绝；
- Google Benchmark 算法矩阵各做 30 个独立样本并记录 P50、P95、最大值和 95% 置信区间；真实全文件索引在 Release 做 3 次、Debug 做 1 次结构运行；
- 隐藏视图测试持续执行共享 FFT，同时拒绝视图专属活动提交。

本地最终结果：

| 预设/范围 | CUDA | 结果 |
|---|---|---:|
| `local-windows-msvc-headless-debug` 全量 | 关闭、无 Qt | 133/133 |
| `local-windows-msvc-headless-release` 全量 | 关闭、无 Qt | 133/133 |
| `local-windows-msvc-cpu-debug` MS-02 确定性集合（完整吞吐/独立基准由上述规范配置覆盖） | 关闭 | 34/34；公共头 1/1 |
| `local-windows-msvc-cpu-release` 同上 | 关闭 | 34/34；公共头 1/1 |
| `local-windows-msvc-cuda-debug` 同上 | 强制 CUDA 12.4 | 34/34；公共头 1/1 |
| `local-windows-msvc-cuda-release` 同上 + CUDA 独立基准 | 强制 CUDA 12.4 | 34/34；公共头 1/1；基准 1/1 |

`local-windows-msvc-cpu-debug`、`local-windows-msvc-cpu-release` 与 `local-windows-msvc-cuda-release` 的 UI/无 Qt 安装消费者均为 2/2。嵌套的无 Qt 包消费者显式继承当前 MSVC、Windows SDK、`VCToolsRedistDir` 和 UCRT 环境，已在普通 CTest 子进程中验证不会因缺少 `LIB` 或运行库目录而误失败。CPU 与 CUDA Release 的洁净 PATH 闭包分别通过：CPU 闭包禁止 CUDA DLL；CUDA 闭包强制包含且只额外加入 `cudart64_12.dll`、`cufft64_11.dll`；两者都不含 SYCL、BLACS、ScaLAPACK、TBB/Intel 线程层或 OpenMP。

当前主机 Release 证据：4,096 点 oneMKL FFT P95 为 30.4 微秒；最终规范全量运行中，剔除全字节 CPU 校验后的真实 X310 完整索引慢侧吞吐为 286,009,000 B/s，纯同盘顺序读取基线为 393,243,000 B/s，比值 0.727307，高于 0.60。最终 Debug 全量运行的持续浏览进程 `PeakWorkingSetSize`：8 GB 与 100 GB 逻辑空间均为 18,382,848 字节，增长 0 字节；该操作系统峰值计数覆盖 `build_frame()` 内部临时缓冲生命周期。以上结果只代表当前主机与当次普通后台负载，不外推为其他设备承诺。测试文件的 SHA-256 为 `74d0ace877568a1c26505c773f249a02e411546254e67c63c4613bb508f2d605`；十进制 100 GB 仅通过逻辑重复映射验证读取计划、重复/EOF 边界和浏览有界性，没有创建或扫描物理 100 GB 文件。按用户决策未执行 Ubuntu 24.04 测试。

实现提交 `f6041d719ec6be9b47eee21eb04addc2a0265704` 的 Windows GitHub Actions 运行 `30331185758` 已完成：`headless-build-test` 与 `windows-ui-module-performance` 均为 success。远程作业验证锁定依赖安装、Windows MSVC 编译、无界面契约、Qt/UI 模块性能冒烟和安装包消费；外部 4 GB 录制与本机 CUDA 实机矩阵仍以本地证据为准。

## MS-03 测试矩阵

MS-03 的 50 项逐需求测试由 Visualization 18 项、时间导航 15 项、Selection/测量 8 项、可用性 7 项和两个平台聚合契约组成：

| 范围 | 需求 | 每个 UI 配置用例数 |
|---|---|---:|
| 图表、视口、图层、截图与显示映射 | `FR-VIS-001`～`FR-VIS-018` | 18 |
| 实际读取范围与时间/频率导航 | `FR-NAV-001`～`FR-NAV-015` | 15 |
| Selection、游标、测量与依赖 | `FR-SEL-001`～`FR-SEL-008` | 8 |
| 键盘、DPI、命中区与可访问性 | `NFR-USA-001`～`NFR-USA-007` | 7 |
| Visualization/Workbench 聚合契约 | `FR-VIS-101`、`FR-WB-101` | 2 |
| 合计 |  | 50 |

每个 UI 构建另注册十三项真实 Qt 回归：1280×720、1600×900、1920×1080、3840×2160，100%、125%、150%、175%、200% DPI，P02/P04/P07 实际 PNG 截图，以及清空三项 Qt 插件环境变量后的默认 Windows 平台启动。P04/P07 页面动作属于 `FR-WB-101` 聚合契约。测试实际创建 Qt Widgets、抓取 Canvas、发送键盘/鼠标/滚轮事件、模拟 DPR 变化并检查可访问属性，不以静态截图或只检查模型字段替代 UI 行为。

交互重点包括：

- 时间导航范围等于实际已读范围，部分数据不扩张到预计总长度；
- PSD 与 STFT 共用精确整数 Hz 视口，滚轮只改变频率横轴；
- 右键正向框选裁剪、反向拖动恢复，平移保持带宽；
- 时域/频谱显示模式切换不改变当前时间范围；
- 旧 `ViewRequestId` 帧不能覆盖新视图；
- 隐藏时域或 PSD 后断开专属观察、准备和绘制；
- Selection 的创建、复制、删除、精确输入、游标测量、通道估算和依赖失效；
- 高频命中区至少 28×28 逻辑像素，高 DPI 键盘操作不丢焦点；
- Canvas 提供可访问名称、描述和等价文本摘要；
- Workbench 命令、面板、布局和宿主内容注入不使用生产伪数据。

本地最终矩阵：

| 预设 | 范围 | 结果 |
|---|---|---:|
| `local-windows-msvc-cpu-debug` | 全量 | 198/198 |
| `local-windows-msvc-cpu-release` | 全量 | 198/198 |
| `local-windows-msvc-cuda-debug` | 63 项 MS-03 + 安装消费 | 63/63；1/1 |
| `local-windows-msvc-cuda-release` | 同上 | 63/63；1/1 |
| `local-windows-msvc-headless-debug` | 无 Qt 全量 | 133/133 |
| `local-windows-msvc-headless-release` | 无 Qt 全量 | 133/133 |

安装消费者通过独立工程消费十个目标、调用 MS-03 公共 API，并在清空 Qt 插件环境变量后启动安装树演示程序。12 个变更 C/C++ 文件通过格式检查；四个生产实现的 `clang-tidy` 均退出码 0、0 errors，共 32 次 warning occurrence，不声称零告警。公共头、不可变基线、外部材料、依赖锁、VS Code、Windows-only CI 和用户预设确定性检查通过。

按用户批准未执行 Ubuntu 24.04 构建测试。MS-03 不重复执行 MS-02 的物理/逻辑大文件性能验证，也不创建物理 100 GB 文件。

2026-07-29 增量门禁在 CPU/CUDA Debug/Release 四套预设上均完成配置、编译、63/63 项 MS-03 CTest 和 1/1 安装消费者。截图脚本的 JSON 清单必须证明物理尺寸等于逻辑尺寸乘实际 DPR；对比脚本必须保留 P02 修复前/后以及 P04/P07 基线/修复后证据。

## MS-04 测试矩阵

MS-04 直接覆盖 20 项批准需求：`FR-INS-001`～`FR-INS-007`、
`FR-EXP-001`～`FR-EXP-009`、`NFR-REL-001`～`NFR-REL-004`。应用层另注册工程导入
分析闭环、取消重试、错误恢复、Qt 前自检、三份外部录制、五档尺寸/DPI、默认 Windows
平台启动和六页截图，共 38 项 `ms-04` 标签用例。

结果测试覆盖 CSV/JSON schema、单位和 provenance，PNG、RAW+JSON、WAV、插件格式，
目录级原子提交、禁止覆盖、批量清单、按工程/源/Selection/Channel 查询、当前/过期
判断和 SHA-256 篡改检测。Inspector 测试覆盖独立通道状态、五类基础容器、星座参数、
眼图适用条件、直方图/瞬时频率单位和预处理、版本过期及缺失插件显式降级。

外部数据只做有界读取并复核源文件：

- 20,480,044 字节 WAV；
- 998,774,448 字节 X310 SC16；
- 4,004,031,888 字节 X310 SC16。

按用户决定不创建物理 100 GB 文件；需要该边界时只对指定 X310 文件执行逻辑重复映射
和有界访问。批准资料未提供 D4，测试报告必须写明“基线未提供”，不能制造或记为通过。

本地候选矩阵：

| 预设 | 范围 | 结果 |
|---|---|---:|
| `local-windows-msvc-cpu-debug` | 全量 | 243/243 |
| `local-windows-msvc-cpu-release` | 全量 | 243/243 |
| `local-windows-msvc-cuda-debug` | MS-04 + Compute | 50/50 |
| `local-windows-msvc-cuda-release` | MS-04 + Compute | 50/50 |
| CPU Debug/Release | 安装消费者与已安装主程序 | 各 1/1 |

`NFR-REL-002` 循环新建、打开、导入、分析、提交和关闭 100 次并检查资源回落；
`NFR-REL-003` 注入权限拒绝、等价满盘提交失败和损坏暂存/缓存，之后复核源文件与已
保存工程；`NFR-REL-004` 校验并篡改结果载荷。`NFR-REL-001` 的普通 CTest 只作为
短冒烟，最终验收必须另执行连续 8 小时真实混合操作进程，并记录起止时间、循环数、
错误流、Working Set、峰值和句柄数后才能收口。

CTest 截图与布局断言使用 Qt offscreen，另清空三项 Qt 插件环境变量走默认 Windows
平台。里程碑视觉证据使用 qwindows 隐藏窗口：先探测本机原生 DPR，再把相对
`QT_SCALE_FACTOR` 归一化为 1.0、1.5 或 2.0 目标 DPR，并校验 PNG 物理尺寸等于逻辑
尺寸乘目标 DPR。安装消费者在独立前缀运行 `SignalStudio.exe --self-test` 和
`--startup-smoke`，同时验证 Qt/VC143 运行时闭包。按用户决定不执行 Ubuntu 24.04
无界面构建，不新增 Linux 兼容声明。GitHub Actions 的 Windows Qt/UI 作业在 MS-03
回归之后显式执行全部 `ms-04` 标签用例，再执行十组件安装消费。

MS-04 最终稳态于 2026-07-29 连续执行 28,800 秒并完成 70,024 次工程新建、导入、
PSD/STFT、结果提交和关闭循环；标准错误为 0，生命周期峰值 Working Set 为
19,476,480 B，采样句柄数为 79～84。最终代码提交 `04932be` 的 GitHub Actions
运行 `30450012411` 中，`headless-build-test` 与
`windows-ui-module-performance` 均为 success。CI 缺少批准外部录制时，外部数据
专项明确跳过，取消/重试使用确定性 SC16 夹具继续执行；本机三份批准录制的有界验证
保持为外部数据验收依据。

## MS-4.5 测试矩阵

MS-4.5 注册 20 项 `ms-4.5` 标签测试：

| 范围 | 数量 | 重点 |
|---|---:|---|
| DSP 数值与后端 | 4 | 参数契约、频谱/PSD、STFT/预滤波、CPU/CUDA |
| 应用核心 | 6 | 参数生效、缓存、最新提交、迁移、来源、参数切换稳定性 |
| Workbench/Qt 合约 | 2 | Inspector extension、Designer 参数面板 |
| 尺寸/DPI | 7 | 1280、1080P、4K、125%、150%、175%、200% |
| 真实截图 | 1 | P03 高级参数面板与真实分析来源 |

DSP 用例使用解析单音、非 bin 对齐单音、多音、分段幅度、脉冲、二次多项式和滤波夹具，
按明确绝对/相对/dB 容差验证八种窗、CG/ENBW、补零、单/双边、幅度/功率/PSD、
Periodogram/Welch、线性/指数平均、最大保持、三种频谱平滑、STFT 二维平滑、FIR/IIR
群时延、取消、序列化和哈希。CPU/CUDA 比较频率轴、幅度、PSD、Welch、STFT、原始
结果和预滤波结果；CUDA 不可用时必须输出 unavailable，不能记为 GPU 通过。

应用用例验证完整参数真实改变 bin/泄漏/估计/平滑/滤波，规范化参数哈希进入缓存和
Artifact；纯显示变化不重算；纯平滑复用 raw；最新 `ViewRequestId` 拒绝旧结果；
工程关闭/重开、用户预设、旧工程迁移、未来主版本拒绝和结果过期均可重复。

CPU Debug/Release 必须执行全量非 benchmark CTest 和 20/20 MS-4.5 专项；CUDA
Debug/Release 在本机可用时执行 20/20 专项。安装消费者、已安装 `SignalStudio.exe
--self-test`、默认 Windows 平台启动、VS Code F5 同构建树、公共头、基线、依赖锁、
格式/静态检查均为收口门禁。

30 分钟专项通过 `scripts/run-ms45-parameter-stability.ps1` 在隔离 Release 进程中连续
切换 Periodogram/Welch、六类累积/平滑组合、FFT/STFT、显示插值、缩放和频率视图，
并周期采样 CPU、Working Set、Private Bytes、句柄和线程。完整 8 小时 MS-04 稳态不
重复执行，但原脚本和回归必须保持兼容。
