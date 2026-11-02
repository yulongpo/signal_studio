# 控件清单

清单来自 HEAD `4904740` 的 `setObjectName` 与构造代码。处置是后续实现目标，不是已完成改动。测试引用只说明源码里有查找，不表示本轮跑过。

## 保留并迁移

| objectName | 当前位置 | 绑定 | 处置 |
|---|---|---|---|
| `waveformMode` | `psdSection` | 显示波形分量 | 只在辅助波形模式的工具栏显示 |
| `psdFft` | `psdSection` | 显示 PSD N | 辅助 PSD 工具栏；驻留帧谱时 N 仍来自 STFT |
| `psdScope` | `psdSection` | 显示统计时间范围 | 辅助 PSD 工具栏 |
| `stftFft` | `specSection` | 显示 STFT N | 主图工具栏 |
| `colormap` | `specSection` | `DisplaySettings.palette` | 唯一色表编辑器，可放到主图工具栏 |
| `dynamic` | `specSection` | 动态范围 | 主图工具栏或锚定弹层 |
| `reference` | `specSection` | 参考电平 | 同上 |
| `autoPowerFit` | `specSection` | 自动适配电平 | 主图工具栏 |
| `freqMode` | 主图图层行 | 频率坐标 | 保留，显示页可到达 |
| `gridToggle` | `specSection` | 网格 | 显示选项，不常驻成长表单 |
| `colorbarToggle` | `specSection` | 色阶条 | 同上 |
| `effectiveBandwidthMHz` | `fileSection` | 文件级有效带宽 | 保留，按文件提交 |
| `manualTool` | 主图标题栏 | 持续标记 | 保留 |
| `runWidebandDetection` | 检测卡 | 提交当前配置 | 检测页固定操作区 |
| `cancelDetectionCardRun` | 检测卡 | `focusedUnfinished()` | 只取消明确聚焦的未结束任务 |
| `deriveWidebandDetection` | 底部任务面板 | `deriveRun()` | 保留派生语义 |
| `detectionRangeOrigin` | 检测卡 | 范围来源 | 保留 |
| `detectionStrategySample` | 检测卡 | Sample | 保留 |
| `detectionStrategyRoi` | 检测卡 | 检测时间策略 ROI | 保留，不与框选 ROI 混名 |
| `detectionStrategyCoarse` | 检测卡 | Coarse | 保留 |

## 删除或替换

`palette` 是主图标题栏的第二套色表下拉，与 `colormap` 写同一 `DisplaySettings.palette`。删除 `palette`，不留隐藏副本。`tests/ui_tests.cpp` 现有查找必须改到 `colormap`。

`detectionProfile` 一个下拉同时放模型默认项和 `profile.id:spectrumMode`。改为两个可见控件：`detectionModel`、`detectionSpectrumMode`。不保留隐藏的 `detectionProfile`。现有 `tests/ui_tests.cpp` 和 `tests/wideband_*_capture.h` 的查找改到新控件，键值仍用现有 `configurationKey`。

这些分段及其 `Toggle`、`Content` 被四个页签替换，测试不再查找它们：

- `fileSection`
- `detectionSection`
- `psdSection`
- `specSection`

`viewSection`、`markSection`、`narrowbandChannelSection` 的内容分别进入显示、标记、通道页。长滚动外壳不保留。`markSection`、`narrowbandChannelSection` 的现有测试查找改到 `contextAnnotation`、`contextChannel` 或内部既有控件。

## 右侧新页签

| objectName | 内容 |
|---|---|
| `contextDisplay` | 文件概要入口、当前视图、频率坐标、网格和色阶 |
| `contextDetection` | 检测卡 |
| `contextAnnotation` | 现有标记检查器与 `renameMark`、`locateMark`、`deleteMarks` |
| `contextChannel` | 现有通道摘要与 `propertyLocateChannelSource`、`propertyEditChannel` |

## 检测卡继续使用的对象名

`detectionCard`、`detectionRangeOrigin`、`detectionActiveSegment`、`selectDetectionRoi`、`detectionRequestedRange`、`detectionFftSize`、`detectionHopMilliseconds`、`detectionModelFrames`、`detectionModelHopFrames`、`detectionPhysicalTimes`、`detectionScoreThreshold`、`detectionNmsIou`、`detectionTopK`、`detectionBandwidthMarginDb`、`detectionTimeStrategy`、三个策略按钮、`detectionSplitBursts`、`detectionSampleEdgeRatio`、`detectionSmoothMilliseconds`、`detectionMinOnMilliseconds`、`detectionMinOffMilliseconds`、`detectionCoarseSplitGapMilliseconds`、`detectionCoarseMinBurstMilliseconds`、`detectionBranchParameters`、`detectionBatchSize`、`detectionMinHits`、`detectionMaxMiss`、`detectionMergeFrequencyHz`、`detectionAdvancedParameters`、`detectionPreflight`、`detectionRunSummary`、`detectionPhaseProgress`、`detectionRunActions`、`viewRunningDetection`、`detectionOtherRun`。

底部任务对象名保留：`detectionPanel`、`detectionRunList`、`detectionRunDetails`、`recoverWidebandDetection`、`detectionStatus`。

## 谱控件

PSD 前缀 `psd`，STFT 前缀 `stft`：`Method`、`Window`、`Overlap`、`SegmentMs`、`KaiserBeta`、`TimeBandwidth`、`Tapers`、`BurgOrder`、`RemoveMean`。PSD 另有 `psdStatistic`。这些名字保留。方法历史只加在对应 `SpectralSettingsWidget` 实例内，不新增工程字段。

## 图谱已有入口

保留 `waveMode`、`psdMode`、`timeFrequencyMode`、`waterfallMode`、`detectionEvidenceMode`、`detectionRoiTool`、`modeMain`、`modeAux`、三个最大化按钮。`modeMain` 与 `modeAux` 可以继续隐藏，只要可见分段按钮仍是同一状态。

检测范围 ROI 继续用 `selectDetectionRoi`、`detectionRoiTool`、`contextDetectionRoiAction`。时间策略 ROI 只用 `detectionStrategyRoi`。两套名字不要合并。
