# Signal Studio UI 设计与实现

## 1. 权威来源与范围

批准的 UI 规范、图表约定、色阶约定、资源、截图和评审原型位于不可变目录
`docs/baseline/Signal-Studio-Dev-Docs/02_原型设计` 与 `03_UI规范`。保留的 HTML 原型只用于历史设计参考，不是运行时依赖，也不会被包装为最终 Qt 软件。

MS-03 交付可复用的 `SignalVisualization` 与 `SignalWorkbench`；MS-04 在其上交付
品牌化 Signal Studio 基础产品、真实工程/导入/分析/结果业务闭环。平台演示程序仍只
使用显式注入的演示数据，最终 `SignalStudio.exe` 则只显示当前工程和实际录制状态。

## 2. 视觉与布局

工作台采用深色工业分析主题：

- 主强调色用于当前工具、Selection 和活动状态，同时使用文字、图标或边框表达状态，不只依赖颜色；
- 主窗口使用菜单、工具栏、中心分析区、可停靠面板和状态栏；
- 中心分析区依次显示紧凑时间导航、时域、PSD 和 STFT，可隐藏时域/PSD并调整图谱高度和位置；
- Inspector 在右侧，任务/结果中心在底部；设置与诊断可复用相同 Dock 体系；
- 控件全部由布局管理器组织，支持 1280×720、1600×900、1920×1080、3840×2160 和 100%～200% DPI；
- 高频指针目标不小于 28×28 逻辑像素，键盘焦点可见且顺序可预测。

十一组真实运行截图、逻辑/物理像素说明和打开方式见
`docs/development/ui-preview/MS-03_UI预览索引.md`。

## 3. 图表与导航

时间导航范围严格等于实际已读样本范围。导航窗口支持整体拖动、两端缩放、滚轮、方向键、PageUp/PageDown、Home/End；部分读取状态同时显示实际字节和预计总字节，不把未读范围当作可用数据。

时域支持实部、I/Q、幅度和相位显示。PSD 纵轴为 `dB/Hz`，显示有效样本数、FFT 帧数、窗函数、平均方法和 RBW。STFT 纵轴为时间，显示窗长、步长、FFT、重叠率、色阶和插值方式。PSD 与 STFT 使用同一频率视口。

频率交互规则：

- 滚轮只缩放频率横轴；
- 平移保持当前带宽；
- 右键从左向右框选频率范围；
- 右键从右向左恢复完整频率范围；
- 支持 Hz、kHz、MHz、GHz 自适应显示和精确整数 Hz 输入；
- 实信号支持单边/镜像双边，复信号支持 `fftshift` 双边和绝对频率。

Selection、游标和测量通过覆盖层模型管理，包含稳定 ID、来源版本、时间/频率范围和依赖关系。删除 Selection 会使依赖测量失效；复制生成新 ID。截图可以独立选择轴、图例、色标、游标、Selection 和参数摘要。

## 4. 工作台

`SignalWorkbench` 提供：

- 服务、命令和面板注册；
- 菜单、工具栏、快捷键和通知；
- Inspector、任务中心、结果中心、设置与诊断；
- 主题令牌和参数单位/范围/默认值/即时校验；
- Dock 布局保存、恢复和持久化；
- 状态栏与可访问状态摘要。

生产工作台默认显示空状态，宿主通过 `WorkbenchContent` 注入 Inspector、任务、结果和状态。示例程序才注入演示录制和任务，避免平台库伪造产品结果。

## 5. Designer 与运行时

以下生产 `.ui` 文件由 CMake 的 `qt_wrap_ui` 实际编译：

- `SignalWorkbenchMainWindow.ui`
- `SignalInspectorPanel.ui`
- `SignalTaskCenterPanel.ui`
- `SignalResultCenterPanel.ui`
- `SignalSettingsPanel.ui`
- `SignalDiagnosticsPanel.ui`

MS-04 最终应用另增加五个生产 `.ui` 文件：

- `SignalStudioProjectHome.ui`
- `SignalImportWizard.ui`
- `SignalLoadProgressDialog.ui`
- `SignalInspectorPage.ui`
- `SignalResultCenterPage.ui`

构建和安装部署匹配配置的 Qt Core/Gui/Widgets DLL、Windows/offscreen 平台插件和 `qt.conf`。清空 Qt 插件环境变量后，程序仍由默认 Windows 平台启动，防止调试器再次出现无法初始化 Qt 平台插件的错误。

## 6. 可访问性与线程边界

Canvas 提供可访问名称、语义描述和等价文本摘要；关键测量可由辅助技术读取。模态对话框锁定焦点，Escape 与取消语义一致。高 DPI 下导航仍可完全通过键盘操作，不因缩放丢失焦点或命中区。

所有 `QWidget` 操作只发生在 GUI 线程。后台 Data/DSP/Compute/TaskRuntime 工作只生成不可变视口和帧结果，完成后由 UI 线程原子绑定；旧 `ViewRequestId` 结果被拒绝。

## 7. 原型页面对齐与 DPI 规则

P02、P04、P07 以 `02_原型设计/页面截图/标准截图` 的 HTML 实际渲染归档为视觉基准。工作台应用外壳固定包含应用菜单条、紧凑命令栏、约 230 逻辑像素左侧导航、中心页签、右侧折叠属性入口和底部状态条。P02 的时域、PSD、STFT 初始比例为 3:4:5；P04/P07 作为完整中心页面，不再以展开 Dock 替代。

所有尺寸、间距和命中区均按逻辑像素表达。窗口处理 `DevicePixelRatioChange` 和屏幕切换事件，更新布局几何与重绘；`QMouseEvent::position()` 保持逻辑坐标，Canvas、QImage 和窗口截图由 Qt 的 DPR 机制生成物理像素。跨显示器切换不得改变逻辑窗口尺寸。1280×720 下可隐藏重复的低优先工具入口，但主要分析区、页签和全局导航必须保留。

## 8. MS-04 产品页面

P01 项目首页显示真实工程、数据源、最近工程和运行环境。W01 导入向导在同一响应式
双列布局中呈现文件/格式与采样/范围约定，文件名提示只有经用户明确确认才会采用；
有界预览由后台任务执行。W05 显示源文件事实、信号格式、采样率、进度和暂停/继续/
取消语义。

P02 继续使用 MS-03 的生产图谱，绑定当前导入的真实时域、PSD、STFT 和同代际视口。
P03 保存独立 AnalysisChannel 的 Inspector 状态；星座图使用独立 I/Q 坐标、零轴和
单位参考圆，眼图缺少符号率/同步来源时明确不适用。P05 以分类、结果列表、来源详情三栏只显示 ArtifactStore 中的真实结果，
支持当前/过期筛选、来源定位和无覆盖导出。产品工作台默认显示 Navigator、
Inspector 与任务中心；用户关闭 Inspector 后显示边缘恢复入口。

自动截图覆盖 P01、W01、W05、P02、P03、P05。W01/W05 截图把主窗口、遮罩和顶层
工作流窗口按 Qt 的 DPR 合成为完整页面，避免只截取对话框后与批准全窗口基线做失真
比较。最终证据通过 Windows 平台探测本机原生 DPR，并把隐藏窗口归一化到目标 DPR，
保留 qwindows 的真实中文字体渲染且不受桌面工作区裁剪。证据覆盖 1280×720、
1920×1080、3840×2160、150% 和 200% DPI，并为六个有标准截图的页面保存并排对比图。
