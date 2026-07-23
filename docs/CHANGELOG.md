# 变更日志

## 未发布

### 新增

- 增加按字节校验的 BL1.0 已批准文档快照与校验清单。
- 增加外部录制数据元信息/校验值和确定性最小 IQ 夹具。
- 增加基于 C++20、MSVC、Ninja、Qt 6.11.1 的 CMake 包及十个公共模块目标。
- 增加稳定的构建/版本、错误/Status、能力和模块描述符契约。
- 增加 Debug/Release 脚本、预设、VS Code 集成、契约测试和安装树消费验证。
- 增加锁定的 vcpkg 依赖元数据以及工具链、许可证审计报告。
- 增加 BL1.0 版本化 C 插件 ABI、最小 C 插件示例及 C/C++ 兼容消费者。
- 增加结构化 `SS-<DOMAIN>-E###` 错误，以及校验、序列化、恢复、标识、原因链和指标引用覆盖。
- 增加真正无 Qt 的无界面构建和组件感知安装包；仅 Visualization/Workbench 消费者发现 Qt。
- 增加机器可读的宿主工具、依赖和离线缓存锁，以及幂等校验和精确缺失材料报告。
- 增加十个具名模块契约、SDK ABI 测试、非发布基准冒烟、公共头文件第三方类型检查、配置路径扫描和便携 Windows/Linux CI 定义。
- 增加可移植无抛出 C ABI 签名、全捕获 C++ 插件异常适配器和故意抛异常的封闭测试。
- 增加十个独立注册的模块性能保护，以及运行全部十项的 Qt CI 作业。
- 增加 BL1.0 依赖元组精确比较、已安装可执行文件哈希和显式获取来源策略状态。
- 增加同进程 Debug→Release 回归驱动，以及幂等、去重的 MSVC/Ninja/Qt 环境导入。
- 增加 GitHub Windows 2022 的 MSVC x64 开发环境显式初始化，以及本地工作流 YAML/ABI 契约校验。
- 增加确定性、原子写入的本机 CMake 用户预设生成和重复生成字节稳定性回归。
- 增加依赖获取、兼容宿主和精确主机快照三种验证模式及注入式负向回归。
- 增加 VS Code 设置/任务/F5 同构建树校验、Windows 根路径语义回归和公共枚举已知值检查。

### 变更

- 按批准后的验证策略将持续集成收敛为 Windows 2022 无界面与 Windows Qt/UI 两项门禁；自 MS-01 起停止 Ubuntu 24.04 无界面构建测试，MS-00 已有运行保留为历史证据。
- 从已提交的预设和 VS Code 配置中移除机器路径；改由脚本向进程环境提供自动发现的工具。
- Core 产品品牌改为中性的 `Signal Processing Platform`。
- 集中实现结构化 Status 不变量，使无效枚举、代码/类别不一致、重试/恢复冲突和畸形嵌套原因在构造时被拒绝。
- 将 vcpkg 离线获取修正为不可变 BL1.0 `.tar.gz` URL，并由两个校验器从已批准获取脚本推导 URL、哈希和文件名。
- 将 `CMakeUserPresets.json` 改为隐藏基预设继承，路径规范化后仅保存一份公共工具链环境，避免递归和重复膨胀。
- 修复首轮 GitHub Actions Windows 失败：Python 验证固定 UTF-8 并兼容传统代码页，最小夹具清单固定排序 JSON/POSIX 路径/LF，MSVC 公共脚本动态发现并复用已初始化环境。
- 将 Qt 契约拆为源码/包最低支持 6.10.3、本机验证 6.11.1 和不可变 BL1.0 port 选择 6.11.1；CI 固定最低版本 `win64_msvc2022_64` 并保存官方元数据证据。
- 修复 Visualization/Workbench 遗留的 Qt 6.11 编译期断言；增加静态契约，防止把本机精确版本重新引入 CMake、包配置、本机发现或 UI 模块最低版本。
- 将 `actions/checkout` 与 `install-qt-action` 固定为完整提交哈希，消除浮动标签并升级 checkout 运行时。
- 记录远程运行 `29919175820` 对修正提交的成功验收：Windows UI 模块/性能作业及 Windows、Ubuntu 两项无界面作业全部通过；早期失败记录继续作为旧提交历史证据保留。
- 将默认 bootstrap 从开发机精确路径/哈希比较改为有界宿主兼容检查；精确版本、路径和文件哈希移入独立快照并改为显式复现模式。
- 统一 VS Code 配置、构建、测试和 F5 使用 `local-windows-msvc-debug`，并为明确平台测试目标复制运行库、拒绝陈旧或错误构建树。
- 修复 `C:\`、UNC 根及扩展长度根被裁剪为非根路径的问题。
- 修复未知 `ModuleId`、依赖模块 ID 和 `CapabilityAvailability` 可越过公共边界，以及 Status 第九次上下文传播抛异常的问题。

### 移除

- 按用户授权的净空策略移除旧上游原型和旧项目文档；其源状态仍可从开发前归档分支/标签恢复。
- 清理 CMake 构建、安装、CTest 临时目录和 Python `__pycache__`；本轮全新验证的结果写入证据后也移除生成构建树。正式基线、外部数据清单与 MS-00 证据日志未删除。
# 2026-07-22：MS-00 最终独立复审收口

- 记录独立规格复审通过及最终代码质量复审无剩余严重或重要问题。
- 记录五项整改验证结果：可移植依赖校验模式、VS Code 任务树一致性、Windows 根路径语义、枚举合法性校验和有界 `Status` 传播。
- 记录本地 Debug 45/45、Release 45/45、VS Code 任务树 45/45、直接 F5 目标以及精确提交 GitHub Actions 三作业成功。
- MS-00 已验收关闭，下一里程碑为 MS-01；整体产品尚未完成。
# 2026-07-23：MS-01 Core/Data/TaskRuntime 功能基础收口

- 新增 SignalCore 统一 `Result<T>`/`Result<void>` 类型化结果与 `Status` 错误模型，构造期拒绝把成功态当作失败结果。
- 新增 SignalData 实/复数容器与零拷贝切片、全部批准 RAW 标量与布局（int8~float64、交叠/平面、IQ/QI、大小端、字节偏移、缩放）、WAV 头解析与文件名参数提示、分块有界读取、有界预览（限定长度结果）、选区导出闭环、多分辨率索引缓存（键/LRU/锁定/损坏恢复/诊断）与标准描述符适配接口。
- 新增 SignalTaskRuntime 有界资源池、优先级、DAG 依赖、暂停/恢复/取消、进度、超时、重试、幂等、视图过期、结构化失败、来源过滤、历史与制品崩溃恢复和观察者。
- 新增 `tests/core`、`tests/data`、`tests/task_runtime` 单元与需求映射用例，平台消费者扩展调用 MS-01 公共 API。
- 新增 `.clang-format`、`.clang-tidy` 与 MS-01 计划/实施/测试/完成证据文档。
- 修复 `scripts/common.ps1` 的 `Update-SignalStudioUserPresets` 未捕获 `VCToolsRedistDir`/`UniversalCRTSdkDir`/`UCRTVersion`，导致裸 `cmake --preset local-*` 无法部署 VC143 运行库的问题。
- 记录无界面 Debug/Release（101/101）与 UI Debug/Release（106/106）四个配置全量 CTest 通过，合计 414/414，需求映射 54/54。
- MS-01 已验收关闭，下一里程碑为 MS-02（DSP 与 Compute 后端）；整体产品尚未完成。
# 2026-07-23：MS-02 DSP 与 Compute 后端

- 新增 SignalCompute：`IComputeBackend`（CPU+CUDA 探测）、`IBufferPool`（预算约束）、`IBackendSelector`（自动选择+显式降级）、`BackendProvenance`。
- 新增 SignalDSP：窗函数（6 种+ENBW/相干增益）、时域统计、IQ 度量、FIR 滤波（4 种响应+跨块状态）、多相重采样、`IFftBackend` 适配器+cuFFT Z2Z GPU 后端、Welch PSD（V²/Hz、单/双边）、STFT（时间/频率轴）。
- `Result<T>` 增加 `operator*`/`operator->`（`std::optional` 语义，向后兼容）。
- CMake：DSP/Compute 新源、条件链接 `CUDA::cudart`/`CUDA::cufft`、`SIGNAL_STUDIO_HAVE_CUDA` 定义、`tests/compute`+`tests/dsp` 子目录；`SignalStudioConfig.cmake.in` 在 CUDA 构建时 `find_dependency(CUDAToolkit)`。
- 记录无界面 Debug/Release（118/118）与 UI Debug/Release（CUDA，130/130）四配置全量 CTest 通过，合计 496/496。
- 记录 oneMKL CPU FFT 后端为环境偏差（本机未安装，不伪造），cuFFT GPU 后端经解析信号验证。
- MS-02 已验收关闭，下一里程碑为 MS-03（Visualization 与 Workbench）；整体产品尚未完成。
# 2026-07-23：MS-03 Visualization 与 Workbench

- 新增 SignalVisualization：IDataSeries（实/谱/谱图/复）、ViewportController（时间/频率共享视口，钳位/缩放/平移/订阅）、IChartView + 5 个真实 QPainter 控件（时域/功率谱/瀑布图/星座图/眼图）、OverlayModel（游标/选区/测量）、ColorScale 显示映射、TimeNavigator、频率单位自适应+Hz 级精度、隐藏停止计算。
- 新增 SignalWorkbench：IServiceRegistry、PanelFactory、ICommandRegistry、ConfigurableDiagnosticsProvider。
- 公共头无 QWidget（native_widget()->void*），Qt 私有链接，依赖 DAG 不变。
- 记录无界面 118/118、UI(CUDA) 142/142 四配置全量 CTest 通过，合计 520/520。
- MS-03 已验收关闭，下一里程碑为 MS-04（Signal Studio 基础应用）；整体产品尚未完成。
# 2026-07-23：MS-04 Signal Studio 基础应用

- 新增 Signal Studio 桌面应用（薄壳）：Qt-free Application 核心（导入/有界读取/PSD/STFT）+ GUI（主窗口/菜单/三控件/导入向导/状态栏）+ Designer main_window.ui。
- 文件名参数解析（cf/sr 单位）、WAV 自动解析、SC16 文件名提示导入、有界窗口读取、隐藏视图停止计算。
- `--self-test` 无头路径不构造 QApplication；应用仅链接公共库，依赖 DAG 不变。
- 修复 RAW 导入 requested_sample_range 缺失导致读取被拒。
- 记录无界面 118/118、UI(CUDA) 150/150 四配置全量 CTest 通过，合计 536/536；外部 20MB WAV 与 1GB SC16 有界读取验证；self-test 通过。
- MS-04 已验收关闭，下一里程碑为 MS-05（宽窄带联动分析）；整体产品尚未完成。
# 2026-07-23：MS-05 宽窄带联动分析

- 新增窄带信道提取（数字下变频：频移+双路 FIR 低通+多相重采样，gcd 约简 L/M）。
- 新增 WidebandNarrowbandController（宽带选区->信道 spec，反转/越界校验，联动状态）。
- 修复 PolyphaseResampler 增益归一化（原 h[i]=v*L 未归一化，下采样幅度被缩放 L/M；改为 sum(h)=L，DC 增益=1）。
- 修复下变频频移符号（原实为上变频移到 2fc）。
- 记录无界面 118/118、UI(CUDA) 152/152 四配置全量 CTest 通过，合计 540/540；解析单音下变频包络稳定验证。
- MS-05 已验收关闭，下一里程碑为 MS-06（PluginSDK/ModelRuntime/Dataset）；整体产品尚未完成。
# 2026-07-23：MS-06 PluginSDK/ModelRuntime/Dataset

- 新增 PluginSDK：PluginHost（ABI-v1 LoadLibrary 加载/query/load/activate/unload，异常边界隔离）、IAlgorithmPlugin + RmsAlgorithmPlugin、discover。
- 新增 ModelRuntime：ModelRegistry（install/resolve/list）、IInferenceSession + NullInferenceSession（ONNX 未装时 unavailable，不伪造）。
- 新增 Dataset：JsonFileDataset（append/query/commit/round-trip，JSON 清单，无第三方依赖）。
- 修复 PluginHost 未调 api.load 获取插件句柄的 bug（activate 用 HMODULE 而非插件句柄）。
- 记录无界面 126/126、UI(CUDA) 160/160 四配置全量 CTest 通过，合计 572/572。
- ONNX Runtime/HDF5 未装为环境偏差，接口就绪不伪造。
- MS-06 已验收关闭，下一里程碑为 MS-07（工程化/打包/文档）；整体产品尚未完成。
# 2026-07-23：MS-07 工程化、打包、文档

- 质量门禁：clang-format --dry-run --Werror 合规（111 源文件已格式化），/W4 编译、DAG 校验、公共头扫描。
- 打包：cmake/Packaging.cmake（CPack ZIP 便携包 + NSIS 若可用）、windeployqt POST_BUILD 部署 Qt 运行库/插件、VC143 运行库部署；SignalStudio-1.0.0-win64.zip（25MB，122 文件）生成并可启动。
- 需求追踪：docs/development/开发需求追踪矩阵.csv 覆盖 198 项。
- 文档：VSCode 构建与调试指南、第三方依赖版本锁定清单、发布检查清单、安装验证报告、ReleaseNotes_1.0.0、LICENSES。
- 记录四配置全量 CTest 通过，合计 572/572；便携包 self-test 退出 0。
- NSIS 未装为环境偏差（仅便携包）。
- MS-07 已验收关闭，下一里程碑为 MS-08（复用证明应用）；整体产品尚未完成。
# 2026-07-23：MS-08 复用证明应用

- 新增 signal_review 无头 CLI 应用，仅链接 Data/DSP/Compute/Core 4 公共模块（无 Qt/无 app-core），
  证明十模块平台可独立复用。
- WAV/SC16 文件名提示导入、有界窗口读取、实/复统计、JSON 审查报告、--version。
- 真实 1GB SC16 有界读取 8192 样本统计通过（totalSamples 249M）。
- 记录四配置全量 CTest 通过，合计 588/588（无界面 130/130、UI 164/164）。
- MS-08 已验收关闭，下一里程碑为 MS-09（最终发布验证与发布）；整体产品尚未完成。
# 2026-07-23：MS-09 最终发布验证与发布

- 最终验收报告（§18 矩阵 35 项）与最终执行报告（§19 28 节）。
- 合并 claude/GLM-sig-studio-dev -> main，标签 v1.0.0，推送默认分支+标签+dev 分支。
- GitHub Release：gh CLI 不可用为权限阻塞，生成 gh release create 命令。
- 35 项验收：28 通过、4 部分（偏差记录）、2 进行中、1 阻塞，无伪装。
- Signal Studio 1.0.0 整体交付：可编译/可调试/可运行/可交互/可测试/可安装（便携包）/可发布。
