# SignalVisualization 公共 API

## 1. 依赖与线程模型

公共头为 `include/signal_studio/visualization/visualization.hpp`，命名空间为
`signal::visualization`。公共契约只使用标准 C++20 和 Signal Studio 自有类型；Qt 是私有实现依赖。调用方在后台生成
`VisualizationFrame`，GUI 线程通过 `IAnalysisWorkspace::bind_frame()` 原子绑定，不允许后台线程访问 `QWidget`。

## 2. 视口与帧

- `ViewportController::bind_source()` 绑定数据源版本、实际已读样本范围、有效频率范围、部分数据标志和字节统计。
- `set_time()`、`move_time()`、`set_frequency()`、`pan_frequency()`、`reset_frequency()` 更新视口并产生新的
  `ViewRequestId`。
- `AtomicFrameCoordinator::begin()` 声明当前请求；`commit()` 只接受同代际、范围一致且结构有效的帧。
- `VisualizationFrame` 同时承载时域、PSD、STFT、星座和眼图数据，以及 PSD/STFT 参数摘要。

频率范围使用 `std::int64_t` Hz，解析和格式化支持 Hz、kHz、MHz、GHz，同时保留整数 Hz 精度。

## 3. 图层、显示与交互

- `LayerModel`：图层显示、透明度、顺序、来源和持久化。
- `DisplayMapping`：线性/对数幅度、自动/手动范围、色阶、参考电平和动态范围。
- `OverlayModel`：时间、频率、时频 Selection；复制、删除、游标测量、通道估算和依赖失效。
- `VisibilityController`：图表可见性、观察连接、准备和绘制计数，隐藏时停止专属活动。
- `ChartLayoutModel`：图表顺序和逻辑高度，最小高度为 96 逻辑像素。
- `ScreenshotOptions`：轴、图例、色标、游标、Selection 和参数摘要的独立开关。

## 4. Qt 工作区工厂

`make_analysis_workspace()` 返回 `std::unique_ptr<IAnalysisWorkspace>`。接口提供本机句柄、视口/帧绑定、交互模式、
Selection 创建、截图、状态文本和可访问摘要。调用方不需要在公共头中包含 Qt。
