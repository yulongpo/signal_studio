# Agent A 集成交接

- 角色：A follow-up，生产集成辅助函数
- 工作树：`D:/project/ss-de-rpo-wt/continue-a`
- 分支：`codex/ss-de-rpo-a`
- 基线：`54d0b34a2c4609838ff857cff09be4423424836b`
- 未推送。未改 D 的 `detection_task` / `detection_source` / `detection_worker_client` / `detection_model_snapshot` / `detection_store`，未改 C 的 UI，未改 domain，未改 CMake。
- 当前树里没有任何调用点。下面的片段是 main 在 D/C 补丁合并后的机械接线，不是已完成的端到端集成。

## 已落地的接口

头文件 `infrastructure/detection_resource_diagnostics.h`。JSON 仍在 Qt infrastructure，使用 `QJsonDocument`。domain 保持无 Qt。不要恢复手写 JSON 扫描器。

```cpp
bool assessDetectionResourcePreflight(const QString& assetDirectory,
                                      const DetectionSourcePlan& plan,
                                      const DetectionRequest& request,
                                      std::string& performanceJson,
                                      QString& error);
bool assessDetectionResourcePreflight(const RuntimeResourceSnapshot& snapshot,
                                      const DetectionSourcePlan& plan,
                                      const DetectionRequest& request,
                                      std::string& performanceJson,
                                      QString& error);
bool assessDetectionResourceStage(
    const char* stage, const DetectionDiskUsage& knownUsage,
    std::uint32_t knownFacts, const RuntimeResourceSnapshot& snapshot,
    std::string& performanceJson, QString& error,
    const RuntimeResourceSnapshot* scratchSnapshot = nullptr,
    std::uint64_t reserveBytes = 0);
```

`stage` 只能是 `"checkpoint"`、`"refine"`、`"publishing"`。事实位：

| 位 | 含义 |
| --- | --- |
| `kDiskFactRawHead` | 已知 raw-head 占用 |
| `kDiskFactResultShards` | 已知结果 shard 总字节和最大单文件 |
| `kDiskFactPublicationShards` | 已知发布 shard 总字节和最大单文件 |
| `kDiskFactScratch` | 已知 refine spill |
| `kDiskFactCheckpoint` | 已知即将写入的 manifest 字节和现有文件字节 |
| `kDiskFactProject` | 已知即将写入的工程字节和现有文件字节 |

缺位表示 deferred，对象里没有 `bytes`。`publishing` 且缺 `kDiskFactScratch` 时 scratch 的 status 是 `not-live`，不收费，也不是 0。已知 0 才写 `bytes: "0"`。`knownPeakComplete == false` 表示仍有 deferred。返回 `true` 只表示这次传入的已知量放得下。

`performanceSnapshot` 会整份重写 `performanceJson` 并丢掉 `resources`。每次它赋值之后，下一次持久化前要再跑 stage helper，把 `resources` 合并回去。schema 仍是 `signal-studio-detection-profile-v1`。诊断拒绝（非法 JSON、schema 不符、合并后超过 65536）返回 `false`，错误 `resource.diagnostics-rejected`，调用方字符串保持原样。资源拒绝会写入带 timeline 的 profile，然后返回 `false`。

目录重载调用 `captureRuntimeResourceSnapshot(dir, nullptr)`。未提供显存时 status 为 `not-requested`，字节为 0。本辅助函数不调用 CUDA，不递归扫描资产，不算哈希，不删除文件。F 的显存证据由 D/main 用已有 `VramProbeInput` 另做一次 snapshot，再把那份 snapshot 传给 stage。

`reserveBytes` 只接收调用方已经持有的保留量，默认 0。`workingSetBudgetBytes`、`evidenceBudgetBytes`、`sampleScratchBudgetBytes`、`rawHeadBudgetBytes` 都是授权或配额，不能当作峰值传入。

## 1. Preflight

位置：`DetectionTaskManager::prepareQueuedRun`。`applyDetectionEvidencePreflight` 仍先执行。retain ON 成功时它可能把 `request.rawHeadBudgetBytes` 写成所需字节。helper 的 request 是 const，必须看到这次更新之后的请求。

失败分支要在后面的 `prepared = {}` 之前返回，并带走可能已更新的 profile。成功时 `resourceJson` 必须放在后一次 `prepared = {}` 之后，因为那次赋值会清掉前面的 `prepared`。

```cpp
#include "infrastructure/detection_resource_diagnostics.h"

if (const auto rejection =
        applyDetectionEvidencePreflight(directory, plan, request);
    !rejection.isEmpty()) {
  prepared = {};
  prepared.run.lifecycle = DetectionLifecycle::Failed;
  prepared.run.computationPhase = "preflight";
  prepared.run.error = rejection.toStdString();
  prepared.run.request = std::move(request);
  error = rejection;
  return false;
}
std::string resourceJson;
QString resourceError;
if (!assessDetectionResourcePreflight(directory, plan, request, resourceJson,
                                      resourceError)) {
  prepared = {};
  prepared.run.lifecycle = DetectionLifecycle::Failed;
  prepared.run.computationPhase = "preflight";
  prepared.run.error = resourceError.toStdString();
  prepared.run.request = request;
  prepared.run.performanceJson = std::move(resourceJson);
  error = resourceError;
  return false;
}
// 原有 executionBatch、contextRange、fingerprint 保持不动。
prepared = {};
prepared.executionBatchSize = executionBatch;
prepared.publicationBudgetBytes =
    request.workingSetBudgetBytes - pipelineReserve;
prepared.run.request = std::move(request);
prepared.run.performanceJson = std::move(resourceJson);
```

判定：

- `plan.estimatedPeakBytes > 0` 时，用该值和 snapshot 的实际可用 RAM 做 `decideResidentAgainstProbe`。同时用 `decideSamplePhaseBudget(workingSetBudgetBytes, estimatedPeakBytes, 0)`。cell 数在这里未知，profile 里 `sampleCells` 为 deferred，没有 `bytes`。
- 授权大于物理 RAM 本身不是拒绝。`workingSetAuthorization.role` 为 `ceiling`，`comparedToAvailableRam` 为 false。
- `Adapted`（`sample.grant-exceeds-contiguous-cap`）返回 true。`Rejected` 的 `sample.chunk-exceeds-grant` 才失败。
- `estimatedPeakBytes == 0` 时不调用 sample 决策，`sourceResident` 为 deferred，没有 `granted` 或 `bytes`。
- retain OFF 且 `rawHeadBudgetBytes == 0`：不乘窗数，`rawHead` 已知字节 `"0"`，`rawHeadProduct.status` 为 `not-taken`。即使 `2044 * 196608` 或乘法会溢出也如此。
- 显式配额：先走原有 `resource.explicit-raw-head-quota` / `resource.explicit-raw-head-cap` 句子。超过 2 GiB 在乘法前拒绝。profile 的 `volumeEnforced` 为 false。卷剩余字节留到 stage，在真正写之前检查。
- retain ON：沿用原有前缀。配额不足、未知卷、卷不足仍是 `resource.raw-head-shortfall`。`volumeEnforced` 为 true。
- legacy combined：返回 true，`rawHead.status` 为 `legacy-combined`，没有 `bytes`。legacy 句子仍只由 `applyDetectionEvidencePreflight` 产生。
- 溢出是 `resource.arithmetic-overflow`，`rawHeadProduct.status` 为 `overflow`，没有 `bytes` 键。错误文本带原前缀和 `overflow the disk quota`，不写 shortfall。
- 本 helper 不重新实现 JSON observation 那句；那句仍在 `applyDetectionEvidencePreflight`。

原前缀：

- 显式配额：`Raw model head disk budget rejected before inference: %1 windows x %2 bytes; limit %3 bytes. Reduce the range, increase the frame interval or explicitly provision a bounded artifact quota.`
- retain 溢出：`Raw model output retention rejected before inference: %1 windows x %2 bytes overflow the disk quota.`
- 显式路径溢出：`Raw model head disk budget rejected before inference: %1 windows x %2 bytes overflow the disk quota.`
- 未知卷：`Raw model output retention rejected before inference: %1 windows x %2 bytes require %3 bytes; remaining disk bytes are unavailable.`
- 卷不足：`Raw model output retention rejected before inference: %1 windows x %2 bytes require %3 bytes; %4 bytes remain.`
- 2 GiB 帽：`Raw model head disk budget rejected before inference: explicit quota %1 bytes exceeds the 2 GiB cap.`

preflight 的 scratch、结果 shard、发布 shard、checkpoint、工程原子保存一律 deferred，没有 `bytes`。`knownPeakComplete` 为 false。

## 2. Checkpoint

权威位置是 `writeDetectionRun`，在 `atomicWrite` 之前。`checkpoint()`、`finish()` 和 `appendDetectionChunk` 都从这里发布 manifest。`DetectionTaskManager::checkpoint` 在 `performanceSnapshot` 之后把 run 交给 `persistRun_` 即可，不要在编码前猜 manifest 字节。

合并 `resources` 会改变 compact JSON 的长度。用即将写入的 `QByteArray` 做账，写回去之后再编码。最多 3 轮。最后一轮的 payload 大小必须等于刚检查过的 `checkpointBytes`，否则失败关闭，不调用 `atomicWrite`。

```cpp
DetectionRun staged = run;
const auto snapshot =
    captureRuntimeResourceSnapshot(directory.toStdString(), nullptr);
const auto relative = manifestPath(staged.request);
const QFileInfo existingInfo(root.filePath(relative));
const auto existingBytes = existingInfo.isFile()
                               ? static_cast<std::uint64_t>(existingInfo.size())
                               : 0;
QByteArray payload;
bool stable = false;
for (int attempt = 0; attempt < 3; ++attempt) {
  payload = QJsonDocument(encodeDetectionRun(staged))
                .toJson(QJsonDocument::Compact);
  DetectionDiskUsage usage;
  std::uint32_t facts = kDiskFactCheckpoint;
  usage.checkpointBytes = static_cast<std::uint64_t>(payload.size());
  usage.checkpointExistingBytes = existingBytes;
  // 下面几项仅在对应字节已经测量时 OR 进 facts。
  // retain OFF 且不是 legacy：facts |= kDiskFactRawHead; storeRawHead 保持 false。
  // retain ON 且 accountedRawHeadBytes 已是本次写盘字节：
  //   usage.storeRawHead = true; usage.rawHeadBytes = staged.accountedRawHeadBytes;
  //   facts |= kDiskFactRawHead;
  // legacy combined：不要置 kDiskFactRawHead，避免写成 not-retained 的 0。
  // chunks 为空可以记结果 shard 的已知 0。
  // chunks 非空时，total 用 accountedChunkBytes，largest 用清单里每个
  // relativePath 的 QFileInfo::size 最大值。两个都有才置 kDiskFactResultShards。
  // 缺 largest 时不要置该位。不要传 sampleScratchBudgetBytes。
  // 不置 kDiskFactScratch、kDiskFactPublicationShards、kDiskFactProject。
  QString stageError;
  if (!assessDetectionResourceStage("checkpoint", usage, facts, snapshot,
                                    staged.performanceJson, stageError)) {
    error = stageError;
    return false;
  }
  const auto next = QJsonDocument(encodeDetectionRun(staged))
                        .toJson(QJsonDocument::Compact);
  if (static_cast<std::uint64_t>(next.size()) == usage.checkpointBytes) {
    payload = next;
    stable = true;
    break;
  }
}
if (!stable) {
  error = QStringLiteral("resource.checkpoint-size-unstable");
  return false;
}
atomicWrite(root, relative, payload);
```

已有 snapshot 上若已有 F 的显存探针，把那份 snapshot 传进来，不要用 `nullptr` 重抓一份把显存写成 `not-requested`。没有显存探针时，上面的 `nullptr` 捕获是对的。

`checkpointExistingBytes == 0` 只表示 manifest 文件还不存在。这是已知 0。scratch 在本阶段保持 deferred。

失败时不要 `atomicWrite`。资源拒绝已经写进局部 `staged.performanceJson`；旧 manifest 留在磁盘上。

## 3. Refine

位置：`infrastructure/detection_time_analysis.cpp` 的 `boundedSample`，现有 lambda：

```cpp
runtime.reserveScratch = [&](std::uint64_t bytes, QString& why) {
  return grant.reserve(bytes, why);
};
```

`bytes` 是流在这次租约里计算出的保留量（计划里的 `scratchReservationBytes` 传到这个回调时就是它）。在 `grant.reserve` 之前做阶段检查。`makePlan` / 该回调还没发生时，不要置 `kDiskFactScratch`。

```cpp
runtime.reserveScratch = [&](std::uint64_t bytes, QString& why) {
  const auto durable = captureRuntimeResourceSnapshot(
      QString::fromStdString(metadata.path).toStdString(), nullptr);
  const auto scratch = captureRuntimeResourceSnapshot(
      runtime.privateScratchRoot.toStdString(), nullptr);
  DetectionDiskUsage usage;
  usage.durableVolumeKey = durable.volume.volumeKey;
  usage.scratchVolumeKey = scratch.volume.volumeKey;
  usage.scratchBytes = bytes;
  std::uint32_t facts = kDiskFactScratch;
  // raw-head / 结果 shard / 已提交 checkpoint 仅在对应 QFileInfo 或
  // accounted* 字段已经测量时 OR 进去。不要加 sampleScratchBudgetBytes。
  const RuntimeResourceSnapshot* scratchProbe = nullptr;
  if (usage.scratchVolumeKey != usage.durableVolumeKey) scratchProbe = &scratch;
  std::string profile = /* job 上 performanceSnapshot 之后的 performanceJson */;
  if (!assessDetectionResourceStage("refine", usage, facts, durable, profile,
                                    why, scratchProbe))
    return false;
  // 调用方若要保留账本，把 profile 写回 run。本回调返回后 grant 仍按原语义租约。
  return grant.reserve(bytes, why);
};
```

`privateScratchRoot` 还不存在时，对已经存在的父目录做卷捕获。卷键不同就必须传入匹配该键的 `scratchSnapshot`。没有这份探针时，helper 返回 `resource.volume-unavailable`，不会把外来 scratch 记到持久卷的剩余空间上。同一卷时传 `nullptr`。

回调里的 `bytes` 是这一次可见的保留量。`ScratchGrant::held_` 和进程级 `scratchLeasedBytes` 不在这个参数里，不要另读它们来编造峰值。

## 4. 最终工程原子发布

位置：`ProjectFileCodec::save`，`bytes` 已经生成、`QSaveFile output(path)` 尚未构造。

```cpp
const auto bytes = QJsonDocument(container).toJson(QJsonDocument::Indented);
DetectionDiskUsage usage;
usage.projectBytes = static_cast<std::uint64_t>(bytes.size());
const QFileInfo existing(path);
usage.projectExistingBytes =
    existing.isFile() ? static_cast<std::uint64_t>(existing.size()) : 0;
std::uint32_t facts = kDiskFactProject;
// candidate 里某个 DetectionRun 的 accountedRawHeadBytes、accountedChunkBytes、
// manifest 的 QFileInfo 大小、发布 shard 的已测量大小，仅在全部已知时 OR：
//   kDiskFactRawHead / kDiskFactResultShards /
//   kDiskFactPublicationShards / kDiskFactCheckpoint
// 不要置 kDiskFactScratch，也不要把 scratchBytes 设成 0 来表示已回收。
const auto snapshot =
    captureRuntimeResourceSnapshot(root.absolutePath().toStdString(), nullptr);
std::string profile;
QString stageError;
if (!assessDetectionResourceStage("publishing", usage, facts, snapshot,
                                  profile, stageError)) {
  error = stageError;
  return false;
}
require(bytes.size() <= 32 * 1024 * 1024, "工程超过 32 MiB");
QSaveFile output(path);
```

检查用的就是这份 `bytes`。不要把 `profile` 写回 `bytes` 或 `container`。写回去会改变 `projectBytes`，使刚做过的空闲空间检查失效。

因此提交进 `.ssproj` 的 performance profile 仍带着 deferred 的 `projectAtomic`，除非在 `encodeDocument` 之前已经把其它已知事实合并进 run。发布阶段的空闲空间门禁是上面这次 side check。`replaceCurrentDetection` 和 `publishPendingStart` 继续调用现有的 `ProjectFileCodec::save`，不需要第二套编码器。

`publishing` 省略 scratch 位时，profile 里 scratch 为 `not-live`。返回 true 且 `knownPeakComplete == false` 表示工程原子对（新文件 + 现有文件）放得下，同时还有别的事实未纳入这次峰值。

## 接线时保持的语义

- 只评估本次传入的已知量或已计划量。授权、整卷配额、默认预算都不是预期峰值。
- 未知量不要写成 0。overflow 和 explicit cap 没有 `bytes` 键。
- 同一卷同一 phase 由现有 `appendPhaseOverlapDiskLines` 做 checked add。shard 单文件大于 8 MiB，或总字节大于 8 MiB 且最大单文件未提供，是 `resource.shard-boundary`，`resource` 为 `format`。等于 8 MiB 合法。
- raw-head 自身超过可用空间是 `resource.raw-head-shortfall`。其它超额是 `resource.disk-shortfall`。未知卷是 `resource.volume-unavailable`。`reserveBytes > available` 是 `resource.reserve-exceeds-available`。
- 每次调用替换整个 `resources` 对象。timeline、schema 以及 `resources` 以外的键保留。stage 之后若还要 preflight 里的 `rawHeadBranch`，在调用前读走。
- 目录重载不请求 GPU。已有 F 探针的 snapshot 原样传入 stage。

## 检查

本工作树 `cmake --preset local-debug`，二进制目录 `out/vs2026-qt611-debug`。构建 `--parallel 2`，目标 `SignalStudioSsDeRpoResourceTests` 与 `SignalStudioSsDeRpoLogicTests`。未改 CMake。未跑 GPU。未写主仓库共享 `out/`。

运行前把 `D:\softwares\Qt\6.11.1\msvc2022_64\bin` 放进 `PATH`，从 `out/vs2026-qt611-debug_bin` 启动。2026-10-28 这次进程的原样结果：

```text
PASS ss_de_rpo_resource_tests failures=0
PASS ss_de_rpo_logic_tests failures=0
```

两个进程退出码都是 0。资源测试同一进程的实机探针：RAM `probed`，总计 67772461056，可用 26728058880；卷 `D:\`，`GetDiskFreeSpaceExW`，可用 1009031761920；显存 `not-requested`。真实临时目录 preflight 的卷是 `C:\`，`GetDiskFreeSpaceExW`，RAM status `probed`。

覆盖保留了原资源测试，并增加：真实临时目录 raw OFF、大授权对照实际 RAM、resident 不足、Adapted 放行、估计超过授权、未知 resident、raw ON 精确卷/差 1 字节/未知卷/配额先于卷、显式配额与 2 GiB 帽、显式配额不在 preflight 卡卷而 stage 卡空闲字节、retain ON 溢出、retain OFF 不乘溢出、legacy、阶段重叠、分卷 scratch、publishing 的 scratch not-live、原子加法溢出、shard 边界、非法 stage、超过 64 KiB 时字符串不变。逻辑测试仍打印 `baseline=82935ab34ec6126707da152bf46d1f352458ab0c`，R-01、R-04、R-04-explicit-256MiB、R-05、R-15、R-12 和两条 publication 前缀均为 PASS。
