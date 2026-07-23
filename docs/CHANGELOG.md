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
