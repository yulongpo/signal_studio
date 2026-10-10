# Signal Studio · A1.4.3 原生界面

本工程从空仓库独立开发，以仓库内的 [A1.4.3 原型基线](docs/prototype/a1.4.3/BASELINE.md) 为依据，实现原型的布局、颜色、图谱方向、默认状态和交互。按用户要求只读参考 `ISA_算法中心重构开发` 的显示抽取、预览及缓存策略，并在本仓库独立实现。旧 Signal Studio 与 ISA 不参与源码、构建或运行。当前分支为 `rebuild/a143-native`。

采用 C++20、Qt Widgets 和 QRhi。Windows 默认使用 Direct3D 11；同一渲染接口可选择 OpenGL，为 Ubuntu 原生支持保留路径。本轮实际构建环境为 Windows x64 / VS 2026 / Qt 6.11.1。QRhi 的底层接口依赖 `Qt6::GuiPrivate`，因此锁定 Qt 6.11.1；升级 Qt 时需要重新编译和验证。

## 当前能力

- 原型菜单、工程树、全局时间导航、波形/PSD 辅助图、时频/瀑布主图、右侧默认展开的参数组及底部三个结果标签；鼠标操作说明位于顶部“帮助”菜单。
- 启动后先进入工程步骤；新建工程会创建独立文件夹并立即写入 `project.json`，打开工程可选工程文件夹，旧版单 JSON 也可导入。工程就绪后添加/打开信号，再进入宽带图谱操作。文件菜单保留“打开演示工程”，`--demo-data` 仍可用于显式载入三份原型演示数据。
- 每文件保存视图、有效带宽、标记及真实窄带通道配置；工程 JSON v3 保存通道视图、独立 PSD/STFT 参数、物理长度和可用前缀，兼容 v1/v2 导入。右侧显示偏好、用户界面几何、侧栏/分组展开状态、底部标签和最近工程由 `QSettings` 保存。
- 主图右键开启持续选择；普通点击选择，框内移动，边线及控制点调整，Ctrl/Shift 多选、批量删除、定位与重命名。
- 空白主图区框选缩放，物理轴滚轮缩放/拖动平移，导航定位，每文件最多 40 条视图历史；主图滚轮 220 ms 合并。Esc 取消拖动并回滚。
- 原生工程 JSON 的完整校验、原子写入和状态往返。样本索引采用 `uint64_t`，JSON 用十进制字符串，频率采用 Hz。
- QRhi 直接绘制曲线顶点、星座点、眼图轨迹、识别时间轴、位流以及功率纹理的颜色映射；坐标和交互文字使用独立覆盖层。颜色变化复用功率矩阵，悬停/驻留变化复用数据纹理和顶点；报告分别记录真实后端、GPU 数据绘制和资源上传。
- 宽窄带统一频段分析：FFT 点数表示当前可见频段的分析点数，频段变窄重新计算并延长所需观测；时间不足时补零并显示实际观测量，补零不提高真实分辨率。点击热图或波形选择实际 STFT 帧，PSD 直接复用该帧的完整数值数据。源索引、显示抽取与实际帧映射分别保留。
- 主图预览与精细显示分级，停止交互 60 ms 后更新精细结果。单后台线程只保留最新请求，按代次取消和拒收旧结果；缓存旧视图按真实交集映射，更新中区域有提示。
- 可导入小端交替 `int16 I/Q` `.iq/.dat/.raw/.bin` 文件；名称包含 `FS...sps` 与 `FC...Hz` 参数，`BW` 可选并作为默认有效带宽。文件以只读内存映射访问。波形显示 ADC 计数等效值，提供 RMS 与峰值保持的瞬时包络；自动适配居中占高 75%，放大后显示逐样本点。PSD、STFT/瀑布由有界后台任务生成，图内缩放和平移沿用原始样本索引与 RF/基带频率。
- 动态范围和参考电平的有效自定义输入即时生效，同步 PSD 纵轴；一次性自动适配读取当前热图与 PSD 的完整功率，上下各留 3 dB。悬停和驻留读数直接以无背景文字绘制在游标单侧，游标与文字按实际背景反色；宽窄带使用相同的轴高亮和鼠标反馈。主窗口使用自定义顶栏按钮，支持空白区拖动/双击和普通窗口边缘缩放。见 [实现说明](docs/architecture/power-and-window-interactions.md) 与 [验收记录](docs/acceptance/display-optimization-2026-10-10/README.md)。

真实 IQ 文件的工程 JSON 保存路径、元数据和每文件显示参数，重新打开时从路径重建图谱；样本数据本身不复制进工程。窄带通道采用真实 DDC、Kaiser FIR 和有理重采样，按需计算波形/PSD/STFT；修改源标记不自动改变通道快照。窄带四页可通过 `--narrowband-demo` 打开。星座、眼图、分类和解调位流仍为明确标注的合成演示；信号检测和设备采集未实现。

## 构建与运行

本机已生成被 Git 忽略的 `CMakeUserPresets.json`，记录 Qt 路径。其他机器可设置 `SS_QT_ROOT` 为安装的 Qt 6.11.1 kit，然后使用公共 presets。

```powershell
.\scripts\build.ps1 -Configuration Debug
.\scripts\build.ps1 -Configuration Release
.\scripts\run.ps1 -Configuration Release
```

构建脚本默认依次执行配置、编译和 CTest。输出目录：

```text
out/vs2026-qt611-debug_bin/SignalStudio.exe
out/vs2026-qt611-release_bin/SignalStudio.exe
```

Windows 自动使用 `windeployqt` 部署运行库，`qt.conf` 指向本目录插件。可直接从对应目录启动。独立运行验证会清理 Qt 环境并将 PATH 限制为系统路径：

```powershell
.\scripts\verify-package.ps1 -Configuration Release
.\scripts\verify-package.ps1 -Configuration Release -SoftwareRenderer
.\scripts\screenshot.ps1 -Configuration Release
```

命令行诊断：

```powershell
.\out\vs2026-qt611-release_bin\SignalStudio.exe --list-screens
.\out\vs2026-qt611-release_bin\SignalStudio.exe --demo-data --screen 2 --full-screen --verify-4k-150 --smoke-test --require-gpu --render-report out/render-report.json
.\out\vs2026-qt611-release_bin\SignalStudio.exe --iq-file "E:\数据集\扫频数据\ALaShan_051\20260805\xiawu\IQ0_FS102.4Msps_BW80MHz_FC830MHz_20260805_154250.dat" --psd-view
.\out\vs2026-qt611-debug_bin\SignalStudio.exe --iq-file "E:\数据集\扫频数据\ALaShan_051\20260805\xiawu\IQ0_FS102.4Msps_BW80MHz_FC830MHz_20260805_154250.dat" --psd-view --iq-interaction-test --screen 2 --full-screen --verify-4k-150 --size 2560x1440 --screenshot docs/acceptance/screenshots/real-iq-debug-screen2-4k-150.png --render-report docs/acceptance/real-iq-debug-screen2-4k-150.json --require-gpu
.\out\vs2026-qt611-release_bin\SignalStudioUiCapture.exe docs/acceptance/screenshots/4k-display2
.\out\vs2026-qt611-release_bin\SignalStudio.exe --renderer opengl
.\out\vs2026-qt611-release_bin\SignalStudio.exe --software-renderer
```

`--require-gpu` 必须识别为硬件设备、成功上传热力图纹理，并通过覆盖层更新时复用纹理的检查。无界面的 Qt UI 测试走软件路径，用于验证事件和业务状态，不能作为 GPU 证据。

本轮显示适配验收仅在 **第二块连接显示器（Redmi 27 NU）** 使用系统实际 **3840×2160 / 150% DPI**，窗口全屏客户区为 2560×1440 逻辑像素。它的 Windows 内部设备路径为 DISPLAY6；设备路径后缀与当前连接序号分别记录。脚本不会修改显示器分辨率或缩放，屏幕条件不符直接失败。报告记录连接序号、屏幕名称、几何尺寸、DPR 和抓图像素尺寸；主屏超大窗口截图不作为该项验收。offscreen 的 150% 逻辑测试只验证布局和交互。

## 模块边界

| 目录 | 职责 |
|---|---|
| `domain/` | 元数据、样本/频率范围、标记、通道、显示状态；无 Qt 依赖 |
| `application/` | 每文件会话、选择、业务命令和视图历史；无控件依赖 |
| `infrastructure/` | 原生 JSON 读写、校验和交替 int16 IQ 文件访问 |
| `app/` | 程序入口、菜单、工程树、参数面板与工作区编排 |
| `ui/charts/` | 坐标映射、手势、IQ 显示抽取、演示绘图、QRhi 渲染及着色器 |
| `tests/` | 状态/存储与 Qt 事件回归 |
| `docs/` | 原型冻结副本、架构、验收与实际验证记录 |

渲染器接收曲线顶点、功率矩阵、颜色表和覆盖层，不读取工程存储或执行信号算法。IQ 文件访问、窄带 DDC 和谱估计位于基础设施侧，在有界工作线程生成数据后提交 UI；检测和持续采集尚未实现。

## 工程格式与验收边界

原生格式为 `signal-studio-native-project`、版本 3，兼容版本 1/2，与浏览器的 `signal-studio-a1.4.3-prototype` 分开。不能直接打开归档的原型演示 JSON；它是产品基线样例。原生工程不包含样本数据、视图历史或业务撤销栈；项目内容仍需显式保存，界面布局偏好与最近工程列表独立写入用户设置。

本轮 Qt 自动测试覆盖原型默认状态、分隔条取消/复位、属性分组前置和手动展开、两套辅助 Y 范围与历史、三图最大化、标记选择/移动/八控制点/取消、重叠对象、2^53 以上坐标以及原生工程往返；对应关系见 [UI 与交互验收](docs/acceptance/ui-parity.md)。浏览器原型的 59 项结果不等于本仓库的测试结果。

2026-10-09 基础 Debug / Release 构建及 CTest 均为 3/3 通过；本轮功能更新后的 Debug CTest 为 3/3，Qt UI 日志为 66 passed / 0 failed / 0 skipped。显示器 2 的本轮 4K/150% 硬件 QRhi smoke 通过，报告与截图位于 `artifacts/feature-update-debug-screen2-4k-150/`。此前显示器 2 的 12 个原生场景、三图最大化/还原回归、Debug/Release 独立包硬件启动和 Release 软件回退均已通过；原始报告与截图见上述验收记录。

显示抽取、缓存与批量线段绘制减少了本机模拟图谱的绘制停顿；同一组普通滚轮观测中，完整事件循环从 198.88–216.64 ms 降到 47.94–62.08 ms。该值包括 GUI 绘制与排队处理，不能换算成 FPS 或真实 IQ 吞吐；测量范围和输入证据见 [显示性能记录](docs/acceptance/display-performance.md)。

此前指定 IQ 文件的读取、图谱显示和第二显示器 4K 验证记录于 [真实 IQ 验收](docs/acceptance/iq-file-int16.md)；不能将该个例外推为不同磁盘、长时间录制或持续采集的吞吐保证。GPU 曲线顶点缓冲已实现，纹理新增行更新和 Ubuntu 部署仍属后续阶段。当前覆盖层先由 QPainter 生成缓存图像，再由 QRhi 合成；字体字形、原生对话框及 hover 阴影与浏览器存在平台绘制差异。

本轮实际结果见 [UI 与交互验收](docs/acceptance/ui-parity.md)，渲染策略见 [渲染架构](docs/architecture/rendering.md)。[基础骨架验证记录](docs/acceptance/verification.md) 和 [骨架验收清单](docs/acceptance/skeleton-checklist.md) 保留为此前阶段资料。

## 独立频谱参数与前缀加载

PSD 与时频图支持 32–65536 点和独立窗/重叠/观测时长参数。PSD 提供周期图、Bartlett、Welch、DPSS 多窗、Burg 及平均/最大/最小统计；新窄带默认完整通道 Welch 平均谱。观测不足时补零，窄带时频图至少 10 帧；保留原时间轴并说明补零与实际重叠，补零不提高真实分辨率。

真实 IQ 顺序读入时，宽带导航显示实际进度和停止按钮；完成或停止后恢复不超过 2048 点的峰值包络。停止后分析和重开工程都受已读前缀限制。可编辑下拉框保留唯一最近自定义值，并在第一项后用分隔线区分预设。

宽带、窄带和通道弹窗的勾选框统一使用灰蓝边框、亮蓝选中底色和白色勾号，悬停、焦点及禁用状态分别显示。

实现契约见 [频谱参数与加载架构](docs/architecture/spectral-settings-and-loading.md)，本轮验证见 [验收报告](docs/acceptance/spectral-loading-2026-10-10/README.md)。
