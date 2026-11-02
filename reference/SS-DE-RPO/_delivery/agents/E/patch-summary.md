# 补丁摘要

分支 `fix/ss-de-rpo-e`。基线 `82935ab34ec6126707da152bf46d1f352458ab0c`。提交说明：`fix(ss-de-rpo): explain durable publication mismatches`。SHA 以该提交的 `git rev-parse HEAD` 为准，不写进本文件，避免提交内容含自身 SHA。

## 行为

`replaceCurrentDetection` 在恢复比较失败时保留两条中文前缀，并在后面写上：

- 磁盘恢复与内存 publication 的 annotation 数量
- 两侧 source set id
- 数量相同则写第一处 `encodeAnnotation` 不同的下标、内存侧 annotation id，以及字典序第一处 JSON 字段
- 来源编码不同则写 `sourceSetEncodingDiffers=1` 和第一处来源字段

`encodeSourceSet`、`encodeAnnotation`、`recoverDetectionPublication`、`loadDetectionRun` / `encodeDetectionRun` 的相等判断都还在。长文本不进入拒绝文案。

跟踪逻辑没有改。预览上限本来就不删除正式 `FrequencyTrack`。频率 bin 索引只写在 `tracking-limit-inventory.md`，没有落地，也没有把 `4000000` 改大。

`beginDetection` 的丢弃语义没有改。

## 文件

- `application/detection_result_service.cpp`
- `reference/SS-DE-RPO/_delivery/agents/E/publication-field-diffs.md`
- `reference/SS-DE-RPO/_delivery/agents/E/tracking-limit-inventory.md`
- `reference/SS-DE-RPO/_delivery/agents/E/ux-r6-contract.md`
- `reference/SS-DE-RPO/_delivery/agents/E/interface-requests.md`
- `reference/SS-DE-RPO/_delivery/agents/E/patch-summary.md`
- `reference/SS-DE-RPO/_delivery/agents/E/agent-status.md`

未改 `domain/detection_postprocess.*`、`infrastructure/detection_projection.*`、`infrastructure/detection_refined_publication.*`、`infrastructure/detection_task.cpp` 和 UI。

## 跟踪逻辑

未改。
