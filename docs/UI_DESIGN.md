# Signal Studio UI 设计与实现

## 1. 权威来源与范围

批准的 UI 规范、图表约定、色阶约定、资源、截图和评审原型位于不可变目录
`docs/baseline/Signal-Studio-Dev-Docs/02_原型设计` 与 `03_UI规范`。保留的 HTML 原型只用于历史设计参考，不是运行时依赖，也不会被包装为最终 Qt 软件。

MS-03 交付可复用的 `SignalVisualization` 与 `SignalWorkbench`；MS-04 才开始品牌化 Signal Studio 产品页面和真实导入业务编排。当前 Qt 界面是可运行的平台原型，具备真实交互、布局、状态和安装运行时，但演示数据由示例程序显式注入。

## 2. 视觉与布局

工作台采用深色工业分析主题：

- 主强调色用于当前工具、Selection 和活动状态，同时使用文字、图标或边框表达状态，不只依赖颜色；
- 主窗口使用菜单、工具栏、中心分析区、可停靠面板和状态栏；
- 中心分析区依次显示紧凑时间导航、时域、PSD 和 STFT，可隐藏时域/PSD并调整图谱高度和位置；
- Inspector 在右侧，任务/结果中心在底部；设置与诊断可复用相同 Dock 体系；
- 控件全部由布局管理器组织，支持 1280×720、1600×900、1920×1080 和高 DPI；
- 高频指针目标不小于 28×28 逻辑像素，键盘焦点可见且顺序可预测。

四组真实运行截图、逻辑/物理像素说明和打开方式见
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

构建和安装部署匹配配置的 Qt Core/Gui/Widgets DLL、Windows/offscreen 平台插件和 `qt.conf`。清空 Qt 插件环境变量后，程序仍由默认 Windows 平台启动，防止调试器再次出现无法初始化 Qt 平台插件的错误。

## 6. 可访问性与线程边界

Canvas 提供可访问名称、语义描述和等价文本摘要；关键测量可由辅助技术读取。模态对话框锁定焦点，Escape 与取消语义一致。高 DPI 下导航仍可完全通过键盘操作，不因缩放丢失焦点或命中区。

所有 `QWidget` 操作只发生在 GUI 线程。后台 Data/DSP/Compute/TaskRuntime 工作只生成不可变视口和帧结果，完成后由 UI 线程原子绑定；旧 `ViewRequestId` 结果被拒绝。
