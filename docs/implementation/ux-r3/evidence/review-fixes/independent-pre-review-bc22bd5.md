# UX-R3 修复代码预审记录

Code-HEAD: bc22bd57458a9c3b0654aa38169dd9ef07c86ce2

日期：2026-10-25。独立 Review Agent 继续承担审核角色，未参与修复。本次按用户要求仅检查 M1/M2/M3 修复及已有补测，不签署最终发布结论，不编辑规范 `INDEPENDENT_REVIEW.md`，不修改实现、测试或交付文档，不 commit/push。

## 当前判断

实际代码已针对原三项问题补上相应机制；下述已完成的原始定向/实屏证据与代码一致。本次限定范围内，没有发现原三个 Major 的触发路径仍未修复，也没有确认新的 Blocker/Major。此判断不代表最终交付已通过，不能用于发布；最终仍须绑定全部验证、文档和证据结束后的精确新 HEAD，另行独立复审。

## M1：可变 manifest 与保存时前缀

阅读 `infrastructure/project_file.cpp` 的 manifest/verifyManifest/save/load 新代码，及 `detection_store.cpp` 的 loadDetectionRun/verifyChunks/verifyChunk/verifyHeadEvidence/visitDetectionChunks 调用链。

- 活动 run 使用明确 `detection-live-v1` 完整性类型，不再把可变 manifest 的瞬时字节 SHA 固定为不可变资产；冻结请求 hash、已保存 minimumChunks 顺序和内容、minimumCompletedWindows/ReadFrames/ResultCount 仍受校验。
- 非终态读取由既有存储层投影为 Interrupted/recoverable；后续 Cancelled 等终态仍可通过同一 live 条目读取。未知 integrity、缺失 run 锚点、请求改变、既存前缀丢弃/重排都会被拒绝。
- 不可变 chunk/heads 仍保留原有完整性验证。后续新增 chunk 不必已出现在保存时的静态清单中，但实际 loadDetectionRun 默认 verify=true，会验证全部当前 chunk；verifyChunk 对 headEvidence 继续检查作用域、形状、字节数和 SHA。外层还遍历当前映射，将后续 chunk/head 纳入文件数和总字节预算。
- manifest 构造仅纳入本工程 run 和已发布存储文件，避免把 worker scratch 或另一个未索引运行当作工程不可变证据。

读新增 `tests/project_file_tests.cpp`：phase-only 更新、保存后新 chunk、Cancelling/Cancelled 后无重存加载、冻结请求改变、已保存前缀丢失、未来 chunk 损坏/缺失、未知规则/缺锚点均有测试；专属 `QProcess` 子进程在保存后再发布 chunk，READY 后只终止该 fixture 子进程，验证 Interrupted、2 个 chunk 和 unread，不是只改 JSON 的“崩溃”模拟。

两配置原始 targeted 日志均实际记录 codec 46/46。未来 heads 的拒绝链还阅读了既有 `adaptedHeadsSurviveReopen`：它修改已落盘 heads 原始字节后要求 loadDetectionRun 失败。尚未把待完成的真实模型“运行保存→继续推进→取消→不重存重开”当作已完成；该流程的 capture 新断言已阅读，最终必须复核实际输出。

## M2：普通保存保留模式和根

阅读 `AssetSaveMode::Preserve`、ProjectFileCodec::save、MainWindow::saveProject/saveProjectAsDialog 和 DetectionTaskManager::Job 目录绑定。

- codec 默认 Preserve；已有容器普通保存从原容器保留 copy/reference 模式及原 assetDirectory 引用，核对会话解析根一致，不自动改为 `<新路径>.assets/...`。
- 普通 Preserve 在原容器缺失/目标改变且已有资产关联时明确失败，要求用户显式选择另存模式。
- MainWindow 当前路径普通 Save 使用 Preserve；不同路径显式复制使用 Copy，并依据目标资产根是否改变保护活动任务。Reference 另存仍保持原根；没有尝试迁移活动 Job 的写入目录。

阅读 `tests/task_ux_tests.h` 的实际 QAction/QFileDialog/伴随资产选择驱动：Running 时 Copy 被阻止，实际 Reference 另存选 No，当前新路径普通 Save 后仍 reference/原根，确认无新 `.assets`、继续增加 chunk 后取消，容器字节保持保存时内容，另一个 MainWindow 重开获得完整 Cancelled 前缀，闲置 Save 仍保留 reference。

`task-ui/Release-Ux-20261025-012833-790.txt` 及 exit-code 为该业务行通过（3 个 Qt 行含 init/cleanup）。首轮 `...012653-399.txt` 真实失败是改名的 terminal-b 夹具 contextRange 超出第二源，修复实际持久 fixture 的上下文，不是放宽判断；原失败仍在。codec 另有引用/改名 Copy 的普通保存与重新打开后保存测试。

## M3：条件判决、参考区域和 I/Q 逆映射

阅读新增 `domain/constellation_observation.h`、NarrowbandWorkspace processingSettings/renderAnalysis、新阶段/参考控件、NarrowbandChart 的异步 reference worker、绘制、等比例坐标逆映射和滚轮入口，以及新 Qt/调制族/原生断言。

- 判决观察为已有真实恢复快照的显示消费者，要求当前真实 timing/carrier 锁定和有效模板/RMS；未锁定、未知假设保留真实散点并给出不可用原因。判决前后 TaggedSymbol 保留同一 output/source 整数及小数位置；明确说明 RMS 对齐约定、用户假设、绝对相位/极性未知、非识别和非位流。
- 前五项来源阶段从同一快照投影；显示阶段/参考/密度/缩放不重复同步。分析代次、源修订和取消仍由既有发布边界约束。
- 最近邻 Voronoi 使用真实模板的距离半平面交集；方格内部可用四邻居，其余边界/交叉 QAM 缺角点用完整竞争约束。几何 worker 有独立取消/代次，显示 settled 等待 referencePending 清除；QPainter 矢量覆盖层没有冒称 GPU DSP 或 GPU 参考区域计算。
- I/Q 读数从当前实际正方形 plotRect 和当前数值范围逆映射，滚轮变更共同 I/Q 范围；离开/取消后 readout valid 清除。

两配置 targeted 均为 modulation 和 NB UI 通过；NB 14 个 Qt 行/12 个业务 data 行，定向未设 W8，不作为最终 W8。数值测试含独立穷举最近点、区域顶点不等式/总面积、全 50 体制阶数、噪声不出判决、定制 APSK；Qt 测试核对同一快照/代次、失效、持久化和实际指针逆映射。

独立读取 Debug GPU/软件 `narrowband-analysis-report.json`：各 26 个唯一场景、PNG 均为 3840×2160，指定 Redmi 27 NU / DISPLAY6 / 1.5。亲自目视 GPU 的 cf32 判决参考和缩放读数截图，确见模板/区域和 I/Q 字样。对两种 renderer 的 CI16/CF32 场景独立计算：QPSK 判决位置与 scale/sqrt(2) 最大差小于 6e-9，source/output 采样网格关系误差为 0，实际缩放后像素逆映射误差为 0。脚本结果位于同目录 `pre-review-bc22bd5-evidence.log`。

缩放场景将四个理想判决点移出可视范围，截图只显示区域和读数，与当前数值范围/裁剪相符；这不等于丢失真实数据。显示数量属性也不能单独证明可视点数量。

## 正式复审前仍待完成/提交的证据

本次不机械重跑全量，也未启动测试进程与正在进行的完整回归/原生任务争用资源；独立工作为代码审查、读取原件、数值重算及目视抽查。

正式复审须取得新最终精确 HEAD，检查完整 Debug/Release 40 项 W8、主 UI 106 与新增窄带 12 业务行的真实逐行覆盖，Release GPU/软件新 26 场景，真实模型活动保存与前缀恢复，最终 12 组合部署和二进制身份。保留首轮 LNK1168/fixture 失败及修复后日志，不能混称同一轮全过。若验证发现问题或代码继续变化，按受影响范围重新核对。

两 Minor 的当前修正文案已读取：完整面积区间、性能未达建议目标和外部分配断言口径均已明确；DELIVERY/VALIDATION 仍是未提交工作副本，不能提前作为最终冻结交付。规范首次审核报告未改动，其 SHA-256 仍为 `65b0643538dd089645c0e627b0ada9b06ad2e51d319b2b257e20e66a1f2aac51`。
