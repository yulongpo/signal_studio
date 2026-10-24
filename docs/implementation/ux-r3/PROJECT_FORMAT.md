# UX-R3 G2 工程容器与独立行为测试

本文描述 `infrastructure/project_file.{h,cpp}` 和 `ProjectStore` 截至修复提交 `bc22bd5` 的工程容器合同，以及分轮测试结果。最终独立 Review、应用工作流与原生验收结论分别见 INDEPENDENT_REVIEW、VALIDATION，不以格式文档替代运行证据。

## 工程身份与布局

用户入口为 `.ssproj` JSON 文件。保存接口要求扩展名为 `.ssproj`，大小写不敏感，父目录必须已经存在；文件名、目录名和项目名不必一致。首次保存为缺少 ID 的项目分配 UUID；后续保存和另存保留 `projectId`。当前 `ProjectFileCodec::load` 依据内容 magic/schema 判断格式，没有强制输入扩展名。

默认 Copy 布局如下，RAW 只是可能的位置，不会由工程保存自动打包：

```text
arbitrary-directory/
  experiment.ssproj
  experiment.ssproj.assets/
    <projectId>/
      annotations/<file-key>/...
      detections/<run-key>/...
  capture.raw                       # 也可以在其它目录
```

`Project::assetDirectory` 是会话中解析出的资产根目录。容器保存这个位置的引用；原生 v6 状态本身不保存 `.ssproj` 的 `projectId`。项目 ID 在新容器层保存，稳定资产 ID 当前由资产相对路径的 SHA-256 得到，并不是每次保存重新生成的随机 ID。

## 容器字段

| 字段 | 当前写入和校验行为 |
|---|---|
| `magic` | 固定为 `SignalStudioProject`。 |
| `schemaVersion` | 数字 `1`；其它版本拒绝。与嵌入状态的 v6 不是同一版本号。 |
| `projectId` | 非空、非 null 的 UUID；保存时沿用已有 ID。 |
| `assetDirectory` | Copy 默认写相对于 `.ssproj` 所在目录的路径；Reference 写绝对资产路径。加载同时支持绝对和相对位置。 |
| `assetMode` | 写入并校验 `copy` 或 `reference`；普通保存保留原模式及原资产引用。 |
| `assets` | 标记、检测分片及 heads 证据为完整文件 SHA-256/长度；活动检测索引采用下述 `detection-live-v1` 冻结请求与持久前缀规则。 |
| `sourceReferences` | 每个非 demo、非 Qt 资源路径文件的外部源引用。 |
| `state` | 完整 `signal-studio-native-project`、`version: 6` 状态对象，由 `ProjectStore::decodeDocument` 校验。 |

容器最多 32 MiB，最多 150 条源引用。资产清单最多 20000 项、累计最多 2 GiB；不代表其它存储层上限放宽。标记存储仍独立限制分片和累计预算，检测存储仍独立校验请求、分片身份和内容。

资产路径必须为 `annotations/` 或 `detections/` 下的相对路径，禁止绝对路径、反斜杠和 `..` 路径分量，并检查真实路径包含关系及符号链接。读取检查每项 ID 与重复路径；不可变项检查长度和 SHA-256，活动索引检查下述冻结请求和持久前缀；原生状态读取器还检查所引用的标记及检测分片。磁盘可以包含未引用临时文件；每个项目 run 仍必须存在清单锚点。

容器 `state` 由 `ProjectStore::encodeDocument` 直接生成内存对象；该接口沿用标记分片写入、检测资产读取校验及完整解码验证，成功后才返回 payload。codec 不再生成、改写或读取中间 `state-v6.json`。代码仍把 `<asset-root>/state-v6.json` 作为虚拟 `assetPath` 传给 encode/decode，仅用于取得伴随资产的基准目录，磁盘上无需存在该文件。加载始终使用容器内嵌状态；旧运行残留的同名文件也不是状态真源，不属于 `assets` 清单。

## 状态、来源和恢复

嵌入状态沿用 v6：多文件元数据、物理长度与已读前缀、独立 PSD/STFT 参数、视图、标记索引、来源和修订、通道创建快照、通道页、检测任务索引、当前文件和通道。大体积标记和检测证据外置。撤销/导航历史不作为工程持久化内容。窗口与全局布局偏好仍由 QSettings 管理，本文不代表应用层的 dirty、最近工程、文件关联或菜单行为已经验证。

源引用写入 `fileId`、`absolutePath`、相对 `.ssproj` 目录的 `relativePath`、十进制字符串 `bytes` 和 `sampleCount`、`sampleFormat`、`sampleRateHz`、`centerFrequencyHz`、`signatureMethod`、`signature`。加载核对引用与嵌入状态的采样解释一致，禁止遗漏真实源或重复源引用。

Windows 跨盘无法形成相对路径，此时 `relativePath` 明确为空，使用经过相同身份/格式校验的 `absolutePath`。不把 `D:/...` 塞进相对字段；同盘仍优先尝试相对引用。2026-10-24 新增绝对引用及 C/D 跨盘实际夹具往返，工程文件独立行为集增为 34 项。

当前签名方法为 `size+first-last-64KiB-sha256-v1`：摘要输入是文件长度的十进制字节，随后是前 65536 字节；长度超过 65536 时，再读取从 `max(65536, size-65536)` 起的最多 65536 字节。小文件覆盖全部内容，大文件中部未采样区域的变化可能不被发现，因此它是重定位用轻量身份，不能描述为整 RAW SHA-256。

解析依次尝试项目相对路径和原绝对路径。候选文件必须通过签名、长度、原 `SampleFormat` 下实际样本数检查。错误的同长度文件不能接收既有标记。所有候选失踪或身份错误时，工程仍可打开，保留标记、通道和检测证据；源设为 `Failed`，可用样本数为 0，并给出重新定位提示。

`relocate` 只在身份及格式检查成功后更新源路径和 fingerprint，不重写标记、通道快照或历史检测请求中的来源路径。当前接口也不执行新的顺序扫描，不增加已读前缀或主动清除先前的失败状态。实际恢复扫描与 UI 状态更新需要应用层另行验证；独立 codec 测试检查恢复路径持久化以及 RAW 可按原格式打开，不把签名检查当作 DSP/导航扫描完成。

## 保存、另存与旧 v6 转换

`save(path, project, error, mode, cancelled)` 默认 Preserve。首次无资产工程创建默认 Copy 根；已有工程普通保存严格保留容器内原 `assetDirectory` 字符串与 `assetMode`，核对会话根一致。已有资产但原容器缺失或目标改变时，要求显式 Copy/Reference，不能隐式迁移。MainWindow 的活动任务保护按实际资产根是否变化判断，同路径保存使用 Preserve。它先用本地 candidate 计算身份、必要时复制检测证据，以 `encodeDocument` 生成内存 v6 状态并写入或校验内容寻址分片，再用 `QSaveFile` 发布容器，最后把 candidate 赋回调用方。已有目标必须通过 magic、schemaVersion 和 projectId 检查，非工程文件及不同项目 ID 会被拒绝。失败通过 `false` 和 `error` 返回。取消可能留下未引用的新分片，不能据此宣称整个目录具有事务回滚能力；本轮验证的是原工程已有字节及调用方 target 不变。

显式 Copy 使用 `<destination>.assets/<projectId>`，保留 `projectId`；检测证据从之前的资产目录校验后复制，标记和来源快照通过存储层重新输出。Reference 必须已有可用资产根并继续引用它。两种模式均不复制 RAW，RAW 仍受独立外部源引用约束。引用模式依赖原资产根目录持续存在。

### 活动检测索引与不可变证据

检测 `manifest.json` 会在阶段、分片发布、取消和终态时原子更新，不能固定其保存时整文件摘要。保存时仍活动的 run 使用 `integrity: detection-live-v1`：锚定 runId 对应目录、冻结请求的 compact JSON SHA-256、已验证的完整 chunk 描述符前缀和 completedWindows/readFrames/resultCount 最低值。加载先用检测存储层验证当前索引、请求、来源、每个 chunk 与 heads SHA，再核对冻结请求相同、保存前缀未删除/替换/重排、计数未倒退；新发布证据仍计入 20000 文件/2 GiB 总预算。未完成生命周期读为 recoverable Interrupted，已经 Cancelled/Completed 则保留终态。项目中每个 run 必须有清单锚点。

标记、chunk、heads 与保存时已终结的 manifest 继续固定完整长度和 SHA-256，旧 schema-1 静态条目保持严格校验。运行临时文件及其它项目共享根下未引用 run 不进入容器清单。新规则不是忽略检测文件变化，也不能追认已因旧版摘要失效且没有冻结请求锚点的工程。API 子进程夹具的中断恢复不等于 TensorRT 进程崩溃验收。

`importLegacy(legacy, destination, target, error)` 为显式转换入口：

1. 目标已存在时拒绝；读取旧文件，原生 `ProjectStore` 仅接受 v6。
2. 从旧文件所在目录解析标记和检测证据；相对 RAW 路径以该目录为基准转换。
3. 复制保存为新的 `.ssproj`，再加载验证，全部成功后才替换 target。

v1–v5 不兼容，不能通过改后缀实现迁移。直接 `ProjectFileCodec::load(project.json, ...)` 不接受旧 v6 原生根对象。转换的预期契约是旧 JSON、RAW、标记与检测资产字节不变；损坏分片不能部分导入。验证涉及对象数量、IDs、精确样本区间、物理频率、标记修订、通道不可变快照、检测请求来源与结果分片，而不只比较界面数量。

## 取消契约与修复验证

`save` 和 `load` 接受可选 `std::function<bool()> cancelled`，默认空。回调返回 true 时应停止，取消读取不得替换 target；取消保存不得发布新容器、修改调用方或改动原工程已有文件字节。允许临时目录里产生未引用的新内容，与改写已有文件是不同问题。作用域结束后取消回调不应影响后续普通操作。

以下保留 v2 失败与 v3 修复后的独立运行证据。codec 修改由主 Agent 完成；本独立测试工作没有代修，v3 沿用相同 33 个用例和严格断言：

| 问题 | 修复与证据 | 独立用例 |
|---|---|---|
| Reference 另存写入原中间状态 | v2 改项目名再另存 Reference，发现原 `state-v6.json` 被改写。主 Agent 改为内存 payload 后，v3 对原工程树的逐字节比较通过。 | `save-as-reference-original-bytes`：v2 FAIL → v3 PASS |
| 晚取消改写原中间状态 | v2 接近发布和末次检查点取消时均发现原 `state-v6.json` 改变。主 Agent 移除中间状态文件写入后，v3 原已有文件字节、容器字节和内存 target 检查均通过。 | `cancel-save-near-publication`、`cancel-save-last-checkpoint`：v2 FAIL → v3 PASS |
| 容器 commit 后才检查取消，早先源码风险 | 末次取消检查已置于 `output.commit()` 前。v3 两个晚取消用例整体通过，取消后的普通加载也通过。 | 两个晚取消用例：v3 PASS |
| 普通保存原先缺少已有目标身份保护 | 已有目标的 magic/schema/projectId 检查已加入；v2、v3 两轮的非工程文件及异工程保护用例均通过。 | `protect-existing-nonproject-target`、`protect-existing-other-project`：v2/v3 PASS |

本次定向测试中没有剩余失败；这不替代最终独立 Review、全量回归或应用层验收。

## 独立测试入口与证据边界

新增 `tests/project_file_tests.cpp` 是 `QCoreApplication` 程序，使用抛出 `std::runtime_error` 的断言，Release 不会因 `NDEBUG` 关闭验证。每个命名用例独立捕获异常，逐项输出 `PASS` / `FAIL`，最后给出计数；任何失败返回 1。本测试不是最终独立 Review。

夹具由 Session demo、QTemporaryDir、4096 个小端 int16 IQ 样本构造；带标记几何修订、修订前通道快照、Eye 页和真实落盘的检测历史/结果分片。demo 与真实源用例均实际生成 RAW，从磁盘文件取得 `iqSourceFingerprint`，使历史检测请求持有不可变来源身份；demo 标志仍决定 codec 是否创建外部源引用。检测 profile 是 `pipeline-fixture-v1`，不代表模型运行或现场 RF 精度。

首次终审前为 35 个用例；修复增加 11 项至 46 项。原始 33 项覆盖直接创建和稳定 ID、移动整个工程及 RAW/资产、另存 Copy/Reference、真实源移动/同长度错误源、失踪与恢复、损坏或失踪的标记/检测资产、错误 JSON/容器/引用、旧 v6 只读转换、v1–v5 拒绝、已有目标保护、无写目录和保存/读取取消；随后增加跨磁盘绝对源引用与旧 v6 转换取消。新项覆盖保存后正常进展/分片/取消、冻结请求篡改、保存前缀丢失、新分片损坏/缺失、清单锚点缺失/未知规则、引用及改名 Copy 普通保存保根、缺失原文件要求显式另存，以及专属 API 夹具子进程被终止后的持久前缀恢复。失败读取、转换和取消比较 target 内存；转换及另存比较原文件树字节。新测试实际结果见 VALIDATION，不因加入用例即宣称通过。

Windows 无写目录用例只针对 QTemporaryDir 下新建的测试目录，用 `icacls.exe` 给 Everyone SID 添加显式拒绝创建文件/子目录，先通过实际写探针验证拒写，再调用 codec；作用域退出移除该临时拒绝项。其它平台使用权限位并同样验证。权限无法建立时用例失败并说明夹具无效，不把可写目录误记为权限边界通过。

集成目标已由主 Agent 添加，测试文件不修改 CMake。目标 `SignalStudioProjectFileTests` 链接 `SignalStudioInfrastructure` 和 `Qt6::Core`，关闭 AUTOMOC，使用现有 `ss_target_defaults`，依赖 `SignalStudio`；CTest 名为 `signal_studio_project_file`，TIMEOUT 为 120 秒。取消用例通过同拓扑成功操作计数再选择末尾检查点，不依赖写死的回调次数。

定向入口：

```powershell
./scripts/build.ps1 -Configuration Debug -Action All -Targets SignalStudioProjectFileTests -TestRegex '^signal_studio_project_file$' -Jobs 4
./scripts/build.ps1 -Configuration Release -Action All -Targets SignalStudioProjectFileTests -TestRegex '^signal_studio_project_file$' -Jobs 4
```

| 轮次 | 实际结果与范围 |
|---|---|
| MSVC `/Zs` 静态语法检查 | 退出码 0；不是链接或运行验证。 |
| 主 Agent `feature-tests-debug-v1`，据主 Agent 反馈 | 15/33；18 项受 demo 检测夹具缺少 `quickFingerprint` 影响，报 `Detection source requires an immutable identity fingerprint`。不能把这些错误直接计为 codec 行为失败。 |
| 本独立测试 `project-file-debug-fixture-v2` | 通过上述 Debug `-Action All` 命令配置、构建成功；CTest 中程序 **30/33**，3 项失败如上，耗时约 9.34 秒，CTest 退出码 8。无写目录、v6 转换、原工程字节检查、损坏资产/容器和已有目标保护实际执行。 |
| 本独立测试 `project-file-debug-codec-v3` | 主 Agent 将 codec 改为 `encodeDocument` 内存 payload 后，按同一 Debug `-Action All` 命令重新配置、构建及运行；包含 `project_file.cpp`、`project_store.cpp`、`main_window.cpp` 等依赖复编。原测试及断言未修改，程序 **33/33**，CTest **1/1**，目标用时约 9.21 秒、CTest 总用时约 9.23 秒，命令退出码 0。 |
| 主 Agent 后续 Release 全量 | `717ab8f` 的 40/40 CTest 中包含当前工程文件 35 个用例；原始 33 项及新增跨盘引用/转换取消均执行。 |

历史失败日志保留为 `out/tasks/wideband-detection/UX-R3/G2/logs/project-file-debug-fixture-v2.log`。v3 完整配置/构建/CTest 日志为 `out/tasks/wideband-detection/UX-R3/G2/logs/project-file-debug-codec-v3.log`；逐项 33/33 的 CTest 原始输出已独立保存为同目录 `project-file-debug-codec-v3-cases.log`，避免后续其它目标运行覆盖 `LastTest.log`。

v3 只证明本程序这 33 个 Debug 独立行为用例通过，不代表全部 G2 验收或全量 CTest 通过。原生 GUI、dirty/最近工程、用户取消对话框、应用层恢复扫描、双击文件关联、4K 实屏及部署不在这个无 GUI 程序的证据范围内。
