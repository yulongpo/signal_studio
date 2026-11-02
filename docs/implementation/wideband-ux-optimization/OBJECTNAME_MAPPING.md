# 对象名对照

原则：能保留的 `objectName` 继续保留，并 reparent 到新位置。必须改名时只保留新控件，不用隐藏旧控件应付 `findChild`。

## 保持不变

`waveformMode`、`psdFft`、`psdScope`、`stftFft`、`colormap`、`dynamic`、`reference`、`autoPowerFit`、`freqMode`、`gridToggle`、`colorbarToggle`、`effectiveBandwidthMHz`、`runWidebandDetection`、`cancelDetectionCardRun`、`deriveWidebandDetection`、`detectionRangeOrigin`、`detectionStrategySample`、`detectionStrategyRoi`、`detectionStrategyCoarse`、`manualTool`。

谱控件前缀 `psd`、`stft` 加 `Method`、`Window`、`Overlap`、`SegmentMs`、`KaiserBeta`、`TimeBandwidth`、`Tapers`、`BurgOrder`、`RemoveMean`，以及 `psdStatistic`，保持不变。

## 替换

| 旧名 | 新名 | 测试改法 |
|---|---|---|
| `palette` | 无。只留 `colormap` | 原 `findChild(..., "palette")` 改为 `colormap` |
| `detectionProfile` | `detectionModel` 与 `detectionSpectrumMode` | 按模型 id 和谱型整型分别设置，不再查找旧下拉 |
| `fileSection` 及 `Toggle`、`Content` | `contextDisplay` 内的按需文件概要 | 不再依赖旧分段 |
| `detectionSection` 及 `Toggle`、`Content` | `contextDetection` | 打开检测改选该页 |
| `psdSection` 及 `Toggle`、`Content` | 辅助图工具栏和谱弹层 | 直接找 `waveformMode`、`psdFft`、`psdScope` |
| `specSection` 及 `Toggle`、`Content` | 主图工具栏和显示页 | 直接找 `stftFft`、`colormap`、`dynamic`、`reference` |
| `viewSection` 外壳 | `contextDisplay` | 视图读数可留内部标签 |
| `markSection` 外壳 | `contextAnnotation` | 内部 `renameMark` 等名字不动 |
| `narrowbandChannelSection` 外壳 | `contextChannel` | 内部通道按钮名字不动 |

`detectionModel` 的项数据仍是 `profile.id`：`ffscn-gui-iq-v1`、`scn-max-psd-v1`、`ffscn-17-experimental-v1`，以及夹具启用时的 `pipeline-fixture-v1`。

`detectionSpectrumMode` 的项数据仍是 `DetectionSpectrumMode` 整型。模型默认是 `0`，不拼后缀。四种谱是 `1` 到 `4`。稳定模型才提供这四项。实验模型和夹具不新增组合。

配置记忆键不变：`configurationKey` 仍是 `profile.id`，或 `profile.id:` 加谱型整型。例如 `ffscn-gui-iq-v1:2`。不要改成新的字符串枚举。

## 不要隐藏旧控件

实现后，窗口内不应再找到 `palette`、`detectionProfile`、`fileSection`、`detectionSection`、`psdSection`、`specSection` 及对应 `Toggle`。测试更新与控件替换同一提交完成，避免半截查找。
