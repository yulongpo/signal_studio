# 控件及作用域清单

以4904740为原始位置，目标位置为本轮要求。控件对象名尽量随真正的唯一编辑器保留。

| 控件 | 原位置 | 目标位置 | 真源/作用域 |
|---|---|---|---|
| waveformMode | 辅助设置 | 辅助波形标题栏 | DisplaySettings；宽带跨文件显示偏好 |
| psdFft / psdScope / psdStatistic | 辅助设置/谱组件 | 辅助PSD标题栏 | PsdSettings；窄带对应活动通道 |
| psdMethod/Window/Overlap/SegmentMs | 辅助谱表单 | PSD设置层 | 独立PSD分析参数 |
| psdKaiserBeta/TimeBandwidth/Tapers/BurgOrder/RemoveMean | 辅助谱表单 | PSD条件表单 | 当前方法生效；历史仅控件内 |
| stftFft | 时频设置 | 主图标题栏 | SpectrogramSettings |
| stftMethod/Window/Overlap/SegmentMs/TimeBandwidth/Tapers/RemoveMean | 时频表单 | STFT设置层 | 独立STFT参数 |
| palette / colormap | 主图/右栏 | 只保留colormap主图编辑器 | DisplaySettings.palette |
| dynamic / reference / autoPowerFit | 时频设置 | 电平设置层 | 已有显示电平状态及实际数据适配 |
| freqMode / gridToggle / colorbarToggle | 图层行/时频设置 | 显示设置层 | 显示坐标、网格/色阶；不改冻结请求 |
| effectiveBandwidthMHz | 文件表单 | 文件属性 | 文件级真实分析频带 |
| manualTool / detectionRoiTool / specMaximize | 主图标题栏 | 主图关键动作 | 工具状态/明确ROI/布局 |
| detectionProfile | 检测卡 | detectionModel + detectionSpectrumMode | 原configurationKey，保持请求等价 |
| detectionRangeOrigin / detectionActiveSegment | 检测卡 | 检测基础区 | visible/available-file/active-segment/custom |
| detectionFftSize/HopMilliseconds/ModelFrames/ModelHopFrames | 检测卡 | 专业分析折叠区 | 下一次DetectionRequest，提交后冻结 |
| detectionScoreThreshold / 三策略按钮 | 检测卡 | 检测基础区 | 阈值/时间策略，不改算法含义 |
| detectionNmsIou/TopK/BandwidthMarginDb/分支参数 | 检测卡 | 后处理折叠区 | 仅当前有效字段；SCN裕量禁用 |
| detectionBatchSize/MinHits/MaxMiss/MergeFrequencyHz | 检测卡 | 高级资源区 | 保留已提交合法值 |
| detectionPhysicalTimes / detectionPreflight | 长文常驻 | 短摘要 + 计算/诊断详情 | 只读实际依据，不伪造精度 |
| runWidebandDetection / cancelDetectionCardRun | 滚动外操作区 | 检测页固定操作区 | 当前配置 / 明确聚焦runId |
| deriveWidebandDetection | 底部任务 | 保留 | 历史按值快照，不自动运行 |
| detectionRunList / detectionRunDetails | 底部任务 | 保留并补键盘 | ID稳定、阶段/进度/结果/证据 |
| annotationSegment 等已有Inspector | 标记分组 | 标记页 | 同一所选ID/活动段、修订/质量/几何 |
| propertyLocateChannelSource / propertyEditChannel | 通道分组 | 通道页 | 活动通道及创建来源快照 |

所有被移动输入保持Enter/Tab/应用内点击提交，失焦不提交；滚轮不改值。所有受影响旧UI及native捕获查找须同步迁移到可见入口，不能保留已淘汰分组骗测试。
