# RAW-IQ 数据格式说明

| 元数据项 | 内容 |
|---|---|
| 文档编号 | SS-DATA-RAW-001 |
| 文档名称 | RAW-IQ 数据格式说明 |
| 项目名称 | Signal Studio / Signal Platform |
| 文档版本 | V1.0.0 |
| 基线版本 | BL1.0 |
| 状态 | 已批准 |
| 内容类型（meta.contentType） | Reference |
| 编制日期 | 2026-07-22 |
| 适用阶段 | 数据实现与测试 |
| 输入来源 | 术语契约、SRS DAT |
| 本版变更 | 冻结 RAW/实复/IQ、范围和校验语义 |

## 1. 适用范围

RAW 是无自描述字节流；IQ/QI 是复信号分量排列，不是全部信号的统称。任何导入必须有 `SignalDescriptor`，不得凭文件名静默猜测影响数值解释的字段。

## 2. 描述符

必填：signalKind、scalarType、componentLayout/order、endianness、sampleRateHz、byteOffset、requestedSampleRange、amplitudeMode/scale。中心频率可为空；多字节需要字节序；单字节为 not_applicable。帧字节数为分量数乘标量字节数，所有范围按完整帧对齐并用 64 位半开样本区间。

```json
{"schema":"signal.raw-descriptor/1.0","signalKind":"complex","scalarType":"int16","componentLayout":"interleaved","componentOrder":"IQ","endianness":"little","sampleRateHz":50000000,"centerFrequencyHz":1245000000,"byteOffset":0,"requestedSampleRange":{"start":0,"end":249693612},"amplitudeMode":"int16_scaled","scaleFactor":0.000030517578125}
```

## 3. 校验与读取

验证文件事实、剩余字节、帧对齐、NaN/Inf/削顶/直流/全零。ReadPlan 的目标为 `min(configuredInitialBytes, remainingFrameAlignedBytes)`。暂停保持边界；取消发布最后完整帧前缀。源始终只读，侧车与缓存不写回源。

## 4. 频率/PSD

实信号默认单边 `0..Fs/2`，复信号默认 `[-Fs/2,Fs/2)` 并可加中心频率。PSD dB/Hz 必须记录窗、ENBW、归一化和参考量；未经标定不得显示 dBm。

## 参考资料

- 原始材料：`../references/`（交付目录之外，只读输入）
- 平台任务提示词（平台化架构版）

## 未决事项

- 无阻断性未决事项；正文中的建议值和待确认项继续按其原状态追踪，不因文档获批而视为已实施。

## 变更记录

| 版本 | 日期 | 变更 |
|---|---|---|
| V1.0.0 | 2026-07-22 | 建立并自动审核通过平台化开发基线，纳入需求、接口、测试和复用边界。 |
