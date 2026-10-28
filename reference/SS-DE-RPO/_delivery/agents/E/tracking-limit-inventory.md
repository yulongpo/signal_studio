# 跟踪上限清单

Agent E。基线 `82935ab34ec6126707da152bf46d1f352458ab0c`。`domain/detection_postprocess.h` / `.cpp` 本轮没有改逻辑，也没有改默认 `4000000`。

## 1. 正式轨迹与预览上限（CONFIRMED_CODE）

`maximumPreviewEvents` 默认 1024，配置校验要求 1…65536。`FrequencyTracker::rememberPreview` 只在 `preview` 日志达到该上限时删掉最旧事件并增加 `droppedPreview`。

正式 `FrequencyTrack` 在 `active` 与 `sealed`。`finish` 从这两处收集 `hits` 达到门槛的轨迹，再按频率、时间、`seedId` 排序写出。它不读预览日志。

`visiblePreview` 保存每条仍在屏上的轨迹的最新几何，日志淘汰不删除该表。`Merged` 会去掉被吸收的 `trackId`。

因此预览事件上限不会丢掉正式 `FrequencyTrack`。已有测试锁住该行为，本轮未改它们：

- `previewQueueDropsOldestWithoutTruncatingFormalTracks`：`maximumPreviewEvents = 2` 时日志为 2，正式轨迹 `hits == 4`
- `closedPreviewSurvivesLogEviction`：日志淘汰后，关闭轨迹仍留在 `visiblePreviewEvents`，`finish` 仍给出 `hits == 2` 且总数为 4

本工作区没有重新配置 CMake，也没有重跑 `tests/detection_postprocess_tests.cpp`。上述结论来自当前源码和这些断言文本。

## 2. 响亮失败，不是静默截断（CONFIRMED_CODE）

超过下列上限时 `consume` 失败并保留失败前已提交的状态。失败帧不会进入 `finish` 的输出。

| 码或文案 | 位置 |
|---|---|
| `association-comparison-limit-exceeded` | 活动轨迹 × 候选，先计数再算 IoU |
| `association-pair-limit-exceeded` | 通过 IoU 门槛的配对数 |
| `frequency-fusion-comparison-limit-exceeded open=… sealed=… comparisons=…` | 开集融合，先计数再判断能否合并 |
| `retained-track-limit-exceeded` | `maximumActiveTracks` 或 `maximumTracks` |
| `track-evidence-limit-exceeded` | `maximumEvidenceBands` |
| `tracker-resident-byte-budget-exceeded` | `stateBytes` 保守上界 |
| `association-resident-byte-budget-exceeded` | 配对表额外上界 |
| `window-candidate-limit-exceeded` | 每窗候选 |

默认值：`maximumAssociationComparisons = 4000000`，`maximumFusionComparisons = 4000000`，`maximumAssociationPairs = 262144`（校验不允许超过 4000000），`maximumActiveTracks = 4096`，`maximumTracks = 32768`，`maximumEvidenceBands = 262144`，`maximumWindows = 262144`，`residentByteBudget = 32 MiB`。

封存会清空该轨迹的 `windows`（逐窗证据），并保留 `hits` 与观测时间。`sealedBurstsDoNotReenterFusion` 明确要求封存后 `windows.empty()` 且 `hits == 2`。这与预览上限无关，本轮不改。

解码器 `topK` 会设置 `topKTruncated`。那是候选解码上限，不是正式轨迹被预览日志丢掉。

## 3. 频率 bin 索引（设计，未实现）

目的：少做显然不能匹配的比较，从而降低撞上 `4000000` 的机会。索引之后若比较数仍超过配置上限，必须继续返回现有错误码，不能为了通过上限而丢掉正式轨迹，也不能只把 `4000000` 改大。

当前循环在 `continue` 之前就 `++comparisons`。一个只访问“有可能匹配”的 bin 索引会少计数，合并赢家在同一候选集合上仍由包含下标 `i,j` 的键决定，访问顺序不改变这个最小值。但它会改变两件现有测试锁住的行为：

1. `trackingLimitsAndCancellationAreAtomic`：`maximumFusionComparisons = 1`，同一窗三个互不重叠载波。第二次比较超过 1，必须以 `frequency-fusion-comparison-limit-exceeded` 开头失败，并且 `finish` 得到空结果。若索引跳过不可能合并的对，这次 `consume` 会成功。
2. `sealedBurstsDoNotReenterFusion`：间隙之后新突发两个开集轨迹，断言 `comparisons == 1`。这两个载波（约 40–42 Hz 与 80–82 Hz）不会合并，但当前循环仍计 1 次。已封存的 30 条不重新进入融合。`finish` 为 32 条。

本轮不实现索引，以免改动顺序、id 或上述失败行为。后续若做，需要同一变更里更新这两处断言，并保持：

- 关联只跳过 IoU 必然低于 `matchIou` 的对；融合的重叠遍只跳过频率间隙必然大于 `mergeToleranceHz` 或不重叠时间的对；碎片遍仍使用现有 `sameCarrier` 与 `fragmentGapSamples`
- 超限仍返回 `association-comparison-limit-exceeded` 与 `frequency-fusion-comparison-limit-exceeded`（后者保留 `open=`、`sealed=`、`comparisons=`）
- 不改 `seedId`、`finish` 排序和合并键
- 不提高默认 `4000000` 来代替索引
