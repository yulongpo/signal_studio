# 变更日志

## 未发布

### 新增

- 实现 MS-03 的纯 C++20 Visualization 公共视口、原子帧、图层、显示映射、Selection、测量、可见性、布局和截图契约。
- 增加真实 Qt Widgets 时域、PSD、频谱、STFT/瀑布、星座、眼图、时间导航和多图联动交互。
- 实现 Workbench 服务、命令、面板、主题、参数、诊断、布局、内容注入，以及主窗口、菜单、工具栏、Dock、Inspector、任务/结果中心、设置和状态栏。
- 增加六个由生产构建使用的 Qt Designer `.ui` 文件、复用演示程序、自动截图脚本和四组尺寸/DPI 预览。
- 增加 50 项 MS-03 需求测试、六项 Qt UI/平台回归及安装消费者的 MS-03 公共 API 与已安装演示启动覆盖。
- 实现 MS-02 的 12 类 DSP 处理链节点、预览、状态延续、取消、下游失效、模板和处理 provenance。
- 增加 oneMKL DFTI 计划缓存、VSL 复数卷积、LAPACKE 带状三角 IIR、解析信号，以及 libsamplerate 两通道有状态有理重采样生产适配器。
- 增加可选 CUDA 12.4 cudart/cuFFT 端到端执行、运行时版本/设备记录、计划缓存、页锁定/设备内存及显式 CPU 降级。
- 增加生产 `IComputeBackend` 能力探测、统一算法操作执行、自动选择、线程保留、预算内存池、后端一致性和可审计降级。
- 增加真实 X310 文件句柄、有界逻辑 100 GB 映射、浏览会话、采样概览、缓存恢复及同代际三图原子帧。
- 增加 Google Benchmark 性能矩阵、Qt 实际绘制专项、`tests/integration/ms02/**` 外部 WAV/SC16 Data→DSP→Cache 验证和 30 项具名需求测试。
- 增加 oneMKL、libsamplerate、CUDA 精确 DLL 闭包、许可证安装及洁净 PATH 验证脚本。
- 增加真实 X310 SC16 的生产全文件索引、完整描述符/文件指纹缓存身份和 Release 同盘吞吐验收；十进制 100 GB 继续只使用逻辑重复映射。
- 补强性能验收：纯顺序读取基线不再执行全字节校验；持续三图浏览采样峰值 Working Set；冷热测试使用远距离不同视窗并拒绝旧代际；100 GB 逻辑映射覆盖重复拼接、EOF 和读取上限边界。
- 实现 MS-01 的 Core 工程持久化、当前上下文、配置、日志、路径、原子文件、SHA-256 和源版本指纹生产契约。
- 实现 Data 的真实/复数样本容器、RAW/WAV/描述符导入、分块与映射读取、有界预览、部分读取、选区导出和格式适配边界。
- 实现时域金字塔、频谱概要、STFT 瓦片、多分辨率键、内存/磁盘 LRU、缓存锁、损坏恢复和可取消预取。
- 实现 TaskRuntime 的有界资源池、优先级、依赖 DAG、暂停/恢复/取消、进度、超时、重试、幂等、视图过期、结构化失败、历史和崩溃恢复。
- 增加 Core 11 项、Data 32 项和 TaskRuntime 11 项具名需求测试，以及 UI、无 Qt、强制 CPU 的 Debug/Release 六套本地验证矩阵。
- 增加 `.clang-format`、`.clang-tidy` 和 MS-01 中文计划、实施及测试证据。

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

- Visualization/Workbench 从模块骨架升级为可安装生产平台；Qt 继续保持私有，公共头不暴露 Qt 或第三方类型。
- Workbench 生产内容改为宿主通过 `WorkbenchContent` 注入；默认显示真实空状态，演示录制、任务和结果只存在于示例/测试。
- Qt 构建树和安装树统一部署匹配配置的 Core/Gui/Widgets DLL、Windows/offscreen 平台插件与 `qt.conf`，清空插件环境变量后仍可直接启动。
- GitHub Windows Qt/UI 作业增加 MS-03 需求集合，Windows 无界面门禁保持无 Qt；不重新引入 Ubuntu 24.04。
- 本机用户预设连续重建保持字节一致，PATH、`CMAKE_PREFIX_PATH`、INCLUDE、LIB 和 LIBPATH 均无重复项。
- Compute 与 DSP 改为共享模块，使 oneMKL/CUDA 保持私有实现依赖，不向公共 C++ 消费者传播第三方链接契约。
- CPU 数值后端固定为 oneMKL sequential/lp64 和 libsamplerate；缺少成熟依赖时明确不可用，不以自研 FFT、滤波、重采样或线性求解静默替代。
- CUDA 策略适配本机已安装的 12.4.131，缺失或实机执行不兼容时记录 `requested=cuda`、`degraded=true` 后降级 CPU；不安装 cuDNN。
- Compute 选择/降级统一检查 Working Set 容量；CUDA 探测、cuFFT 计划与内存分配器绑定实际设备并恢复调用线程设备。
- 一致性 provenance 只接受独立参考结果计算的最大绝对误差和均方根误差；没有独立参考的固定零指标明确标记为未验证。
- 修复处理链旁路顺序、实际输出实/复契约、细粒度取消、PSD/频谱 bin 与有限值校验，以及超大 FFT 频率映射边界。
- Qt 性能测试目标随构建部署 Windows/offscreen 平台插件，修复 Debug 直接启动时无法初始化 Qt 平台插件的问题。
- 增加清空三项 Qt 插件环境变量后的默认 Windows 平台自动化启动回归。
- 修复嵌套无 Qt 安装消费者未继承 MSVC、Windows SDK、VC143 运行库与 UCRT 环境而在普通 CTest 子进程中误报配置失败的问题。
- 本机用户预设继续按规范化、大小写不敏感规则去重，并为显式 CUDA Debug/Release 增加可直接运行的测试预设。
- 将 TaskRuntime 确立为任务生命周期、进度、重试和日志的唯一状态源；Data 只发布不可变数据范围及可用性。
- 取消数据读取时仅发布已经形成的完整帧前缀；截断、描述符矛盾、WAV 对齐或字节率错误继续按解析失败处理。
- 恢复任务重试改为按 `task_type` 使用当前注册处理器，并为重试及其依赖创建新的 TaskId 和 attempt；缺少处理器时明确失败。
- 公共 C++ 消费接口传播 `/utf-8`，UI 消费运行时显式加入 Qt 目录；Debug nonredist 运行库只用于本地测试且不进入正式安装，Release 安装继续部署匹配的 VC143 可再分发运行库。
- 修复任务重试读取视图代际的数据竞争、旧视图制品提交竞态和并发观察者事件乱序；大制品同步与摘要阶段不持有视图锁，最终 rename/登记保持原子保护。
- 将任务 journal 升级为 SSTJ3，持久化依赖与超时并兼容 SSTJ2，使崩溃恢复后的 DAG 重试和同幂等提交保持原定义。
- 为 WAV 导出增加整数采样率、32 位表示、byteRate、blockAlign 和 RIFF `36 + data` 上限校验。
- 强化本机用户预设生成：公共环境只保存一次，PATH、`CMAKE_PREFIX_PATH`、INCLUDE、LIB 和 LIBPATH 全部规范化去重，不读取旧生成文件。
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

- 清理已被 CPU 专用预设替代的通用 Debug/Release 构建树、依赖模式临时目录和 MS-02 临时重建日志。
- 按用户授权的净空策略移除旧上游原型和旧项目文档；其源状态仍可从开发前归档分支/标签恢复。
- 清理 CMake 构建、安装、CTest 临时目录和 Python `__pycache__`；本轮全新验证的结果写入证据后也移除生成构建树。正式基线、外部数据清单与 MS-00 证据日志未删除。
# 2026-07-22：MS-00 最终独立复审收口

- 记录独立规格复审通过及最终代码质量复审无剩余严重或重要问题。
- 记录五项整改验证结果：可移植依赖校验模式、VS Code 任务树一致性、Windows 根路径语义、枚举合法性校验和有界 `Status` 传播。
- 记录本地 Debug 45/45、Release 45/45、VS Code 任务树 45/45、直接 F5 目标以及精确提交 GitHub Actions 三作业成功。
- MS-00 已验收关闭，下一里程碑为 MS-01；整体产品尚未完成。

# 2026-07-26：MS-01 验收收口

- 记录 Core、Data、TaskRuntime 共 54 项需求，以及 UI、无 Qt、强制 CPU 的 Debug/Release 六套全新矩阵全部通过。
- 记录独立规格复审和最终代码质量复审通过；并发视图、制品提交、事件有序性、崩溃恢复、WAV 边界和 Debug nonredist 发布整改无剩余 Critical/Important。
- 记录实现提交 `39f1d0f2ae9b2cc063543cbdbe69bc3ddd388fd2`、远程整合提交 `c89412e615168b067f3f29646e778b6de5c8b1b5`，以及 Windows GitHub Actions 运行 `30187026089` 两项作业成功。
- MS-01 已验收关闭，下一里程碑为 MS-02；整体产品尚未完成。

# 2026-07-28：MS-02 验收收口

- 记录 DSP、Compute、CPU/CUDA、真实外部数据、逻辑 100 GB、Qt 默认 Windows 平台启动、安装消费者及洁净运行时闭包全部通过。
- 记录最终无界面 Debug/Release 各 133/133、四套 CPU/CUDA 确定性矩阵各 34/34，以及整改后独立复审无剩余 Critical/Important。
- 记录实现提交 `f6041d719ec6be9b47eee21eb04addc2a0265704` 与 Windows GitHub Actions 运行 `30331185758` 的两项作业成功。
- MS-02 已验收关闭，下一里程碑为 MS-03；按用户边界，MS-03 完成后暂停，不进入 MS-04。

# 2026-07-28：MS-03 验收收口

- 记录 Visualization、Workbench、真实 Qt Widgets 原型、六个 Designer 文件、50 项需求和四组尺寸/DPI 预览全部通过。
- 记录 Qt 构建/安装运行时与默认 Windows 平台插件故障修复、安装消费、本机预设确定性和六套本地构建测试矩阵。
- 记录实现提交 `a4d3a763eece76c966a3763b5831cccc98baee84` 与 Windows GitHub Actions 运行 `30350430444` 两项作业成功。
- MS-03 已验收关闭；按用户边界暂停，获得确认前不进入 MS-04。
