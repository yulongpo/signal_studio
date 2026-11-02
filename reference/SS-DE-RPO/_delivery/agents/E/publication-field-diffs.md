# Publication 字段差异诊断

Agent E。基线 `82935ab34ec6126707da152bf46d1f352458ab0c`。比较仍在 `DetectionResultService::replaceCurrentDetection`。本补丁只加长两条拒绝文案，不删除 `loadDetectionRun` / `encodeDetectionRun`、`recoverDetectionPublication`、`encodeSourceSet`、`encodeAnnotation` 相等判断。

## 1. 拒绝前缀（必须原样开头）

| 条件 | 前缀 |
|---|---|
| `encodeSourceSet` 不同，或 annotation 数量不同 | `持久检测来源/对象数与 publication 不同` |
| 来源编码相同、数量相同，且某一处 `encodeAnnotation` 不同 | `持久检测对象与 publication 不同` |

后缀以空格开始，因此整句仍以原前缀开头。数量不同时不扫描 annotation，也不写 index。

`recovered*` 来自 `recoverDetectionPublication`（磁盘）。`publication*` 是 `replaceCurrentDetection` 收到的内存 publication。

## 2. 后缀

始终包含：

- `recoveredAnnotations=<数量>`
- `publicationAnnotations=<数量>`
- `recoveredSourceSetId=<id>`
- `publicationSourceSetId=<id>`

来源编码不同时再加 `sourceSetEncodingDiffers=1` 和 `firstDifferingSourceSetField=<路径及叶子>`。

数量相同且存在 annotation 编码差异时再加：

- `firstDifferingAnnotationIndex=<从 0 开始的下标>`
- `annotationId=<内存 publication 该下标的 id>`
- 两侧 id 不同时加 `recoveredAnnotationId=<磁盘 id>`
- `firstDifferingField=<路径及叶子>`

对象键按字典序取第一处不同；数组按下标取第一处不同。键里非字母数字、下划线的字符在路径里写成 `_`。id 里的空白写成 `_`。路径超过 180 或深度达到 12 时以 `depthLimit=1` 停止。

叶子：

- 短标量（null / bool / 数字 / 不超过 64 且不含空白、`=`、`"`、`%` 的字符串）：`recoveredValue=` 与 `publicationValue=`
- 更长的字符串：`recoveredFieldBytes=` 与 `publicationFieldBytes=`（UTF-8；超过 100 万 UTF-16 码元时数字后加 `utf16`，不再转码），并加 `contentDiffers=1`。字节数相同只说明长度相同，内容仍然不同。
- 数组长度或类型不同：`recoveredArrayCount=` 与 `publicationArrayCount=`，并加 `contentDiffers=1`
- 对象在叶子停止：`recoveredObjectKeys=` 与 `publicationObjectKeys=`，并加 `contentDiffers=1`
- 一侧缺键：`recoveredPresent=` 与 `publicationPresent=`（0 或 1）

不把 `originalText`、`supplementalOriginalText`、`originalFieldsJson` 的正文写入拒绝文案。

## 3. 比较面（CONFIRMED_CODE）

`encodeSourceSet`（`infrastructure/annotation_json.cpp`）包含：`id`、`fileId`、`sampleChannel`、`sourceKind`、`formatId`、`parserId`、`parserVersion`、`profileId`、`profileVersion`、`originalPath`、`originalText`、`sourceHash`、`supplementalOriginalText`、`supplementalHash`、`coordinateInterpretation`、`runId`、`sourceIdentity`、`diagnostics`、`deleted`。

`encodeAnnotation` 包含：`id`、`name`、`fileId`、`sourceSetId`、`originRecordId`、`sourceKind`、`classLabel`、`note`、`activeSegmentId`、`revisionId`、`segments`、`observation`、`observationFrequencyFrame`、`observationTimeQuality`、`originEvidence`、`reviewStatus`、`reviewRevisionId`、`reviewHistory`、`revisions`、`derivedFrom`、`conversionSnapshot`、`deleted`，以及仅在为真时的 `plotHidden`。

字典序上 `supplementalHash` 先于 `supplementalOriginalText`。两边补充文本不同时，第一处字段通常是 `supplementalHash`，短十六进制会直接写出两侧值。

## 4. 持久与瞬态边界（CONFIRMED_CODE）

`detectionModelEvidenceIsDurable` 为 `legacyCombinedEvidence || rawHeadBudgetBytes > 0`。`preserveDurableModelEvidence` 在该条件不成立时直接返回，不调用 `preserveRuntimeEvidence`。

`storeDetectionHeadEvidence` 只在 raw-head 落盘路径把 `modelEvidence` 写进 chunk mapping。`execute` 仅在 `legacyCombinedEvidence || rawHeadBudgetBytes > 0` 时调用它。

`recoverDetectionPublication` 用 `sourceSetForRequest` 重建来源，再对窗口 chunk 的 `mappings[0].modelEvidence` 调用 `preserveRuntimeEvidence`。证据对象为空时该函数返回，不改补充文本。

内存 publication 的来源同样从 `sourceSetForRequest` 开始，只在 durable 条件成立时逐批覆盖补充文本。因此非持久的 last-inference 不应再单独造成 `encodeSourceSet` 不一致。这是当前源码的结构，不是本工作区的实机复测。

历史日志里 `159/159` 之后出现过原前缀失败（`03_AUDIT_BASELINE.md`，CONFIRMED_LOG，针对审查时的旧记录）。本工作区没有重放该日志，不能把旧记录写成当前仍稳定复现。下一次失败会带上数量、source set id 和第一处字段。

## 5. 窗口完成不是 Completed（CONFIRMED_CODE）

`DetectionTaskManager::execute` 在窗口循环里增加 `completedWindows`，并写入 `DetectionCoverageKind::Completed`。频率收尾、时间精修、`fuseDetectionPublication`、最终 shard 仍可能 `finish(..., Failed)`。

`finish(..., Completed)` 只出现在最终 shard 写完且 `previewFinal = true` 之后。`finish` 若已被取消则记 `Cancelled`；manifest 持久化失败则把 lifecycle 改成 `Failed`。本补丁不改 `detection_task.cpp`，也不因为窗口数到齐把运行标成 Completed。
