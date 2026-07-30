# SignalWorkbench 公共 API

## 1. 依赖与边界

公共头为 `include/signal_studio/workbench/workbench.hpp`，命名空间为 `signal::workbench`。公共契约依赖
Visualization、TaskRuntime 和 Core，但不公开 Qt 类型。Qt Widgets 主窗口由私有实现创建。

## 2. 注册表与平台服务

- `ServiceRegistry`：以稳定 ID 注册和解析宿主服务。
- `CommandRegistry`：注册命令 ID、文本、快捷键、启用状态和处理器，并按 ID 调用。
- `PanelRegistry`：注册 Dock/中心面板描述符，约束区域、顺序、默认可见性和稳定对象名。
- `IDiagnosticsProvider`：返回后端、环境和问题快照。
- `ThemeTokens`、`ParameterDescriptor`：统一主题与带单位/范围/默认值的参数契约。

## 3. 布局与内容

`WorkbenchLayout` 通过 `serialize_layout()` 和 `parse_layout()` 保存/恢复 Dock 状态。损坏输入返回结构化失败，不破坏上一有效布局。

`WorkbenchContent` 由宿主注入：

- Inspector 属性；
- 任务中心任务；
- 结果中心结果；
- 当前状态和诊断状态。

默认内容为空，因此平台库不会伪造录制、任务或结果。演示程序可以显式构造演示内容。

`IWorkbenchWindow::install_inspector_extension()` 允许最终应用把宿主拥有的参数页面
安装到 Inspector。公共方法只接收不透明原生句柄，Qt 类型仍留在私有实现；Workbench
不拥有 DSP 参数，也不在后台任务完成前伪造结果。

## 4. Qt 工作台工厂

`make_workbench_window()` 接收 `WorkbenchConfiguration`、一个 Visualization 工作区和可选命令/面板注册表，返回
`std::unique_ptr<IWorkbenchWindow>`。接口提供显示、布局恢复/保存、可访问摘要和本机句柄；Qt 仍完全隐藏在实现侧。
