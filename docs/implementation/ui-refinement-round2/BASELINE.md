# 基线与已定位问题

源码0f108d0，两个主程序SHA与上轮交付一致。身份见out/tasks/ui-refinement-round2/01-baseline/diagnostics/baseline-identity.json。上轮两配置28/28、原生57/57为复用的已归档基线结果，本轮没有重复宣称执行。

- ChartControlStrip的stretch位于控件后面，导致左排；overflow把combo转换为带QMenu的按钮，引入嵌套菜单。
- 窄带全局条混合波形/PSD/STFT；shared高级设置入口在右栏。点击图谱后的帧谱模式禁用PSD N，但无明显切回统计谱入口。
- 窄带refresh用findData恢复N，合法的预设外工程值找不到时空白或回退。观察页小合成星座与说明约占1/4空间。
- DDC长计算状态重复进入波形/时频/全局状态，还覆盖recognitionStatus；识别未运行就显示18/QPSK/0.82，权重哈希为空和synthetic=true常驻。
- 工程树名称、物理信息和技术ID混杂，活动文件与通道颜色不一致；元数据额外占树层级，标记批量区说明冗长。

基线截图引用上一轮screenshots/channel-ddc.png、after-detection.png，原始报告和实际显示屏身份保留；本轮验收重新生成独立报告。
