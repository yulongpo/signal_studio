# 图谱渲染后端

本阶段选择 Qt Widgets + QRhiWidget，将后端与图谱交互、业务坐标及工程状态分开。该选择按 Windows 与 Ubuntu 的后续交付目标确定。用户给出的[“介绍 Direct2D”选型会话](https://chatgpt.com/share/6ac7b6db-aa24-83e8-9334-526fa8dc83da)是决策背景；本轮已确认采用 QRhi。

## 平台与兼容范围

Windows 使用 QRhi Direct3D11；Ubuntu 预留 Qt 默认 OpenGL 后端，Vulkan 可在后续环境验证后选择。同一顶层窗口的图谱保持同一种 API，避免窗口合成冲突。Qt 官方说明了这些默认后端及单窗口约束，见 [QRhiWidget](https://doc.qt.io/qt-6.11/qrhiwidget.html)。

QRhiWidget 属于公开 Widgets API，但其使用的 QRhi/QShader 等 Gui 类没有通常的源码与二进制兼容保证，需要 `Qt6::GuiPrivate`。本仓库以 `find_package(Qt6 6.11.1 EXACT ...)` 锁定开发、ShaderTools 与运行库版本；升级 Qt 后须整体重新编译并验证各后端。依据见 [QRhiWidget 的兼容说明](https://doc.qt.io/qt-6.11/qrhiwidget.html)。

Windows 的 VS 2026 presets 与输出部署用于本机验证。`portable-ninja` 提供无 VS 路径的配置入口，以 `SS_QT_ROOT` 找到平台对应的 Qt 6.11.1 安装；Ubuntu 构建、插件部署、驱动兼容与 GPU 验收尚未在本阶段完成。

```bash
export SS_QT_ROOT=/path/to/Qt/6.11.1/gcc_64
cmake --preset portable-ninja
cmake --build --preset portable-ninja
ctest --preset portable-ninja
```

Ninja 构建目录为 `out/portable-ninja`，Debug 可执行目录为 `out/portable-ninja-debug_bin`。需要 CMake 4.2+、Ninja、C++20 编译器及同版本 Qt 私有开发头文件和 ShaderTools；Linux 依赖部署后续单独实现。

## 当前绘制路径

设计参考 ISA 的可见像素预算、交互预览与静止重绘思路；抽取器、异步任务和 QRhi 显示路径在本仓库独立实现，构建输入保持本仓库源码及明确安装的工具。历史项目的构建或性能结果不能用作本工程的验收结果。

图谱保留物理时间/频率业务坐标。演示文件的主图使用模拟功率矩阵；真实 int16 IQ 文件则在后台计算 STFT，再转为有限尺寸 Indexed8 色彩图。热图上传为独立纹理；文字、网格、标记边界、曲线与游标由 QPainter 在 CPU 上绘制到 RGBA 覆盖层，QRhi 将两张纹理合成到窗口。`AcceleratedSurface` 按热图 revision 控制像素上传，覆盖层变化和纹理位置/UV 变化复用已有热图纹理。图形设备保持不变的普通 resize 保留热图资源；设备变化时重建资源。硬件 QRhi 成功证明驱动、上传及合成生效，曲线与文字的栅格化工作仍在 CPU。

顶点与片元着色器位于 `ui/charts/shaders/`，分别由应用与 UI 测试的独立 `qt_add_shaders` 资源目标生成并内置为 `:/signalstudio/shaders/texture.vert.qsb` 和 `texture.frag.qsb`。同版本 Qt 的默认 qsb 配置包含 SPIR-V、GLSL、HLSL 5.0 和 MSL。当前不启用 `PRECOMPILE`，无需为普通 shader 构建额外要求 FXC 位于 PATH；后续可明确启用原生预编译。相关命令说明见 [qt_add_shaders](https://doc.qt.io/qt-6/qt-add-shaders.html)，本机生成规则以 Qt 6.11.1 的 `Qt6ShaderToolsMacros.cmake` 为准。

## 抽取与业务坐标

`ui/charts/display_sampling.cpp` 的 `extremaEnvelope` 将不可变曲线输入按可见物理像素列分桶，每桶保留最小值与最大值，按原输入索引顺序输出，并保留首尾有效点。输出至多约 `2*columns+2` 点；非有限值跳过，全无有效值时为空。桶划分使用商与余数累加，避免大索引乘法溢出。波形与 PSD 复用缓存的源曲线，再按像素预算抽取；导航路径按尺寸与演示种子缓存。

真实波形每个显示位置读取最多 1024 个相邻复采样点，输出 IQ RMS 幅度 dBFS；PSD 按当前时间窗或选中标记分成最多 64 个 Hann 窗做 Welch 平均，然后映射到可见频率。导航预览对全文件做同样的有限样本抽取。三种曲线请求均由单独工作线程执行。演示辅助曲线源点数仍为 `clamp(round(plotWidth*12),4096,65536)`。静止显示列数为 `floor(plotWidth*DPR)`，交互预览减半。源数据、抽取索引和绘图缓冲分别缓存；改变辅助 Y 仅更新坐标缓冲，游标或选框刷新不重新生成源曲线。辅助图与导航图按相邻点缓存 `QLineF`，使用 `QPainter::drawLines()` 批量绘制。曲线路径与线段计数由 `curvePathElements`、`curveRasterMethod` 和 `drawnLineSegments` 报告。

同文件还提供 `peakReduce2D`：二维源矩阵按不重叠矩形取有限值最大值，以保留窄峰；无有效值的矩形输出 NaN，参数非法、数据不足或请求放大时返回空。演示矩阵预览可复用更细缓存矩阵；真实 STFT 每个像素频带取对应 FFT bin 的峰值，并按最多 800 万复采样点的 FFT 工作预算减少独立时间列，再扩展至显示栅格。

抽取仅改变显示缓冲。原始文件元数据、uint64 `SampleIndex`、半开时间范围、标记/通道范围、选择与历史保持业务原值；显示桶索引不得代替业务样本索引或写入工程 JSON。命中和拖动仍在原业务视图上，以整数时间原点加相对偏移计算，保留 `2^53` 以上小窗定位精度。

## 60 ms 预览与异步生命周期

视图变化立即进入 `preview`，并重启 60 ms 单次计时器；稳定后切到 `settled` 并请求细化显示。主图当前预览栅格为宽 `clamp(plotWidth*.20,120,300)`、高 `clamp(plotHeight*.20,90,180)`；静止栅格为宽 `clamp(plotWidth*.65,120,750)`、高 `clamp(plotHeight*.8,90,430)`。实际 IQ 矩阵与曲线计算均限制显示栅格/FFT 窗数量；60 ms 只是停止交互后的请求延迟，实际细化完成还取决于任务执行、排队回调与帧提交。

主图只有一个 `HeatmapWorker`：一个进行中任务，最多一个待执行的最新请求。新的请求覆盖待执行请求，并通过单调 generation 使旧任务在每个栅格行退出。任务持有文件元数据、视图、模式、尺寸及 generation 的副本，工作线程不读取 `Session` 或 QWidget。GUI 接收结果时再次核对 generation 与完整缓存 key；跨文件、模式或视图的迟到结果丢弃。控件析构停止 worker 并 join，避免状态层销毁后仍被后台访问。

已完成的功率矩阵缓存最多四份；功率 key 含文件、种子、视图、模式和尺寸，颜色 key 独立含配色、动态范围与参考电平。颜色变化复用已有功率结果，覆盖层变化复用热图纹理。同尺寸与 DPR 复用覆盖层 QImage；若先前上传仍共享图像，Qt 可能执行 detach。游标/悬停值不变时不再次通知覆盖层失效。细化等待期间，已有同文件同模式热图按新视图映射并剪裁，通过纹理 UV 复用；切换文件或主图模式时不将旧热图展示到新上下文。

硬件图谱的 `cpuWallTimings` 分开记录覆盖层图像准备、QPainter 绘制、纹理上传入队、render 与包含 Qt 提交的完整 paintEvent。每项包含 `lastMs`、`maxMs`、`totalMs`、`samples`；这是 GUI 线程墙钟时间，完整 paintEvent 包括 Qt/驱动处理，也可能含等待，不是 GPU 时间戳。原生滚轮诊断同时记录事件处理与 `processEvents` 实际耗时，避免把输入槽返回时间当作画面刷新时间。测量对照见 [显示性能记录](../acceptance/display-performance.md)。

`isDisplaySettled()` 要求细化模式、当前 generation 已提交、worker 空闲、矩阵 key 对应当前文件/模式/请求；真实辅助图还要等曲线后台任务完成。启用硬件 surface 时也须无待上传热图/覆盖层。`renderStatistics()` 提供栅格、缓存、丢弃请求、曲线与纹理计数及 `lastMatrixMs`。时间只反映当前图谱矩阵任务，不推导持续播放吞吐、帧率或检测性能；真实 IQ 的可见峰值与大文件验收见 [真实 IQ 验收](../acceptance/iq-file-int16.md)。

## 下一阶段边界

曲线可以转为顶点缓冲与独立 QRhi 管线，使时间范围变化主要更新坐标变换，数据变化才上传必要的曲线片段。时频/瀑布可以使用分级瓦片、可见区域纹理与环形历史，新帧只更新新增行；缩放期间提供对应分辨率的纹理，避免反复上传整幅高分辨率图像。

基础 int16 IQ 读取、PSD 与 STFT 已接入；仍未包含 DDC、检测或连续采集。真实 IQ 通过只读 QFile 内存映射访问，图谱工作线程持有路径、元数据与视图快照，按文件/视图代次丢弃迟到结果。GPU 曲线几何、连续输入队列、实时新增行更新和 Ubuntu 部署需实现并单独验收。

## 验收证据

本轮显示适配目标仅为 Windows **当前连接的显示器 2、3840×2160 原生像素、150% DPI、全屏**，对应 2560×1440 逻辑客户区。本机目标为 Redmi 27 NU；Qt `QScreen::name()` 返回此友好名称，而 GDI 原生路径实测为 `\\.\DISPLAY6`，反映 Windows 历史设备编号。用户指定的“显示器 2”不要求 GDI 路径为 DISPLAY2。`--screen 2` 与 `ui/display_target.h` 按当前连接屏幕清单的 1-based `connectedIndex` 选择第 2 块活动屏，记录友好名称与真实原生路径；目标不可用或尺寸/DPI 不符时失败，禁止回落到主屏。基础 CTest UI 用例在 offscreen 软件回退下验证上述逻辑尺寸的交互；共用 `showWindow` 的 Windows 交互用例按同一连接编号进入显示器 2 全屏，实际显示器、DPI 与 GPU 证据独立保留。

本机首显过程观察到 QRhiWidget 重建顶层 surface 后丢失提前设置的屏幕目标。公共 `showFullScreenOnScreen` 先建立可见窗口，100 ms 后重新设置目标 screen/native handle 与目标几何，再进入全屏；应用与 Windows UI 回归共用此路径。验收依据是完成后的实际 screen、原生像素、DPR 与全屏状态，不能仅因请求了目标屏就记为通过。

`scripts/gpu_smoke.ps1` 从对应 Debug/Release 包目录以 Windows 平台、system-only PATH 运行，清除 Qt 插件搜索与缩放覆盖变量，保持系统显示设置。先调用 `--list-screens --render-report <path>` 直接保存 `connectedIndex`、屏幕友好名称、真实原生 `deviceName`、逻辑几何、物理像素与 DPR，避免依赖 GUI 子系统的 stdout 捕获；再传入 `--screen 2 --full-screen --verify-4k-150 --require-gpu`，保存 JSON 与 PNG。通过须同时满足连接编号 2、原生尺寸/DPR、全屏、3840×2160 抓图、非 CPU QRhi 驱动、纹理上传、帧提交及覆盖层复用；本机 Redmi 27 NU 型号仅记录为实际设备信息，不作为跨机器型号限制。`-SoftwareRenderer` 另存软件证据，仅证明 QWidget 回退。

```powershell
./scripts/gpu_smoke.ps1 -Configuration Debug
./scripts/gpu_smoke.ps1 -Configuration Release
```

脚本语法检查与实现审计不能代替上述实际运行。GPU 记录还应包含 Qt 版本、backend、驱动/设备、上传/帧提交计数及退出码；最大化重建、预览/静止、快速切换和异步取消的回归结果分别保留。新后端或 Qt 升级后重新收集，最终结果见 `docs/acceptance/`。
