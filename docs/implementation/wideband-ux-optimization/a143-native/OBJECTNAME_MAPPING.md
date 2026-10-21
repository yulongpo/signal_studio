# 对象名迁移

| 原对象 | 新对象/位置 | 验证要求 |
|---|---|---|
| palette | 删除；只保留colormap | 全窗口无palette旧编辑器，色表与Session一致 |
| detectionProfile | detectionModel / detectionSpectrumMode | 模型id与枚举0–4映射原configurationKey |
| 七段长表单 | contextDisplay / contextDetection / contextAnnotation / contextChannel | 先通过可见页签进入再操作内部编辑器 |
| psdFft / psdScope / psdStatistic | 辅助PSD标题栏/溢出 | 模式可见性、统计与驻留帧语义 |
| waveformMode | 辅助波形标题栏 | 非波形态不可混入 |
| stftFft / colormap | 主图标题栏 | 高优先级、单行可用 |
| dynamic / reference / autoPowerFit | 电平层 | 打开层后才编辑，真实提交及滚轮检查 |
| freqMode / gridToggle / colorbarToggle | 显示层 | 与右键和工程恢复同一状态 |
| psd* / stft*方法参数 | 对应设置层 | 保留对象名、按算法隐藏表单行 |
| effectiveBandwidthMHz | 文件属性 | 通过可见入口打开，不向旧侧栏发送事件 |
| runWidebandDetection / cancelDetectionCardRun | 检测页固定操作区 | 页面可见、可点击，runId明确 |
| manualTool / detectionRoiTool / 最大化 | 保留关键动作 | 右键/标题栏同步，位置命中不被遮挡 |
| annotationSegment / Inspector内部对象 | 标记页 | 原编辑语义及断言不删 |
| 通道内部对象 | 通道页 | 来源定位与参数编辑可达 |

新增设置层/工具栏/页签对象名随W2/W4实现补齐；不得残留隐藏旧编辑器以通过findChild。既有UI、截图捕获、原生journey查找应同提交更新到真实入口。未动的业务编辑器内部对象名保持。
