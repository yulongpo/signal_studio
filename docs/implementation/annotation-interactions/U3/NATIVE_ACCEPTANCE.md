# U3 原生交互验收

使用 `scripts/screenshot.ps1 -Configuration Debug -AnnotationInteractions U3`。
本轮最终采集为 r7，12 个场景全部通过；最终归档见
[运行报告](native-report.json)及[产物身份](artifact-manifest.json)。

实际显示器为 U27U2D、连接序号 1、`\\.\DISPLAY2`，3840×2160、
150% DPI，主窗口实际全屏 2560×1440 逻辑像素。每场景检查窗口
所在屏、实际截图尺寸和 DPR。对话框截图为该屏的原生裁剪。
图谱使用 QRhi D3D11，报告分别保存硬件驱动、帧和纹理上传证据。

| 场景 | 核对行为 | 证据 |
|---|---|---|
| 01 | 原始 v5 字节备份、重复保存不增生备份、旧通道快照不重算 | [截图](screenshots/01-v5-upgrade-preserves-backup-and-channel.png) |
| 02 | 质量菜单为可选判断，不作为通道门槛 | [截图](screenshots/02-optional-quality-menu.png) |
| 03–04 | 表格入口、待判断对象当前第 2 段、11–13 ms 裁剪及真实 CPU DDC | [表单](screenshots/03-pending-located-channel-trim-form.png)、[结果](screenshots/04-pending-located-real-cpu-ddc.png) |
| 05–06 | 左树入口、已忽略且时间未定位、人工 4–8 ms、真实窄带显示 | [表单](screenshots/05-ignored-unknown-user-range-form.png)、[窄带](screenshots/06-unknown-user-range-real-ddc-narrowband.png) |
| 07–08 | 空交集仍能打开配置，具体诊断；显式选择前缀后执行尾部 DDC | [空视区](screenshots/07-empty-visible-range-requires-explicit-prefix.png)、[前缀](screenshots/08-partial-prefix-tail-real-cpu-ddc.png) |
| 09 | 图形入口，物理来源改变时指出来源字段并阻止发布 | [截图](screenshots/09-physical-source-diagnostic.png) |
| 10 | 多选要求显式指定一个来源，取消不创建 | [截图](screenshots/10-multiple-selection-explicit-one-source.png) |
| 11 | 名称、备注、质量及兄弟段变化不使通道失效；绑定几何/删除/撤销更新关系 | [截图](screenshots/11-bound-geometry-relations-and-snapshot.png) |
| 12 | v6 重开保留三来源原始证据、未知时间及人工范围，DDC 输出哈希一致 | [截图](screenshots/12-v6-reopen-preserves-user-range-and-real-ddc.png) |

三次 DDC 分别生成 8000、8192、8192 个有限且能量非零的实际 IQ
输出，核对提取范围、输出范围和可读前缀，并记录输出 SHA-256。
`LocatedSegment` 保存实际段绑定；`UserRange` 不伪造 `segmentId`
或算法活动段。原始标签直接读自真实 SigMF meta，手动及算法标记
是明确的交互夹具；本节点不执行检测模型，不声明模型效果通过。

场景 07–08 的界面使用受控可用前缀，不能代替真实停止加载。
真实 SourceLoader 停止、Loading 非空前缀、越界及缓存拒绝读取
首个未读样本由处理器和 Qt 回归单独覆盖。

菜单入口通过公开 Qt 鼠标上下文事件打开可见控件的真实菜单，
再点击启用动作；没有调用业务槽替代交互，也不声明完成物理输入
设备测试。v5 夹具保持原 W: 来源，仅验证迁移与快照；实际 DDC
另用用户提供的本机同哈希样本。
