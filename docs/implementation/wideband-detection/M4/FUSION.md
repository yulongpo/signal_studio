# M4 细化后的事件去重

独立 C++20 domain 实现位于 `domain/detection_refined_fusion.h/.cpp`，不依赖 Qt。数学依据是本节点 `VALIDATION_PROTOCOL.md` 的 coarse/拼接/去重规定及 M0 冻结的 `business/tracker.py`：`deduplicate_refined_blocks` 调用 `stitch_temporal_fragments(max_gap_s=0)`。实际参考目录不存在 `business/fusion.py`，未使用或复制该路径的源码。只读重新核对 tracker SHA-256 为 `77c36a2b7f584c560e2109c97a78ff1688b527ace9765b736941657f0e862ed1`，与 M0 `assets.json` 一致。

## 语义与来源

输入是同一 source/channel/run 发布范围的 `DetectionRefinedEvent{segment,parentIndex,stableId}`。只接收真实测量的 `TimeQuality::AlgorithmEstimated` 活动段；unknownTime/窗口观察没有活动段，不进入该 API。各事件的频率坐标系须相同，稳定 ID 必须唯一。跨文件/来源/运行的隔离由调用方在构建这一个发布范围时保证。

两个时间半开区间必须有正重叠或端点相接；uint64 比较不将样本转成秒，任意正长度静默 gap 保持分段。参考秒坐标存在 1e-12 s epsilon，本实现不借 epsilon 桥接真实的一个样本 gap。频率交集须严格为正；同载波满足 frequency IoU≥matchIou，或 containment≥matchIou 且 centreDistance≤0.5×min(widths)+2×binWidthHz。频率接触不能代替频率交集。

每轮选择 `(-max(IoU,containment), centreDistance, minStartSample, i, j)` 最小的配对，i/j 为仍有效的原始输入索引顺序。由于允许的时间 gap 均为 0，参考 key 的 `max(0,timeGap)` 恒为 0；没有删去参与排序的有效量。合并频率/时间物理边缘并重新判断，直到没有 admissible pair。最终按频率下沿、时间首端、频率上沿及原始索引排序。

输出 `DetectionRefinedFusion` 保存合并后的 Segment 和排序后的 `memberEventIndices`；每个索引都指向不可变输入数组，调用方可恢复其父候选和原始 stableId。内部用成员链表合并，不复制丢弃 parent lineage。合并仍为 `AlgorithmEstimated`，uncertainty 取最大，clippedAtRoi 用 OR，混合频率质量降为 ModelEstimated，新的合并段未确认。算法来源 r0、原始成员几何/精修状态及唯一窗口 union 由调用方在 durable publish 前包装；该 domain 函数不修改原始测量或已保存 Annotation。

## 边界与验证

hard limits 为 100000 input events、4000000 累计 active-pair comparisons、32 MiB working bytes 与每个 ID 256 bytes；配置可收紧。工作内存预算预留 nodes、成员链接、排序索引、最坏原子输出、字符串内容/分配余量，并包含最终 swap 前仍驻留的旧 output。超预算、不合法输入或取消均保持 output 和 summary 原样；不会先发布部分合并结果。取消在 bounded stage 间及每 512 comparisons 检查。`estimatedWorkingBytes` 是含分配余量的保守计算，不是实测 RSS。

2026-10-14，独立 MSVC 14.51 C++20 `/W4 /Od /fp:precise` 构建无警告，`tests/detection_refined_fusion_tests.cpp` 的 9 组行为测试全部通过。日志 `out/tasks/wideband-detection/M4/logs/fusion-standalone-msvc.txt`，可复现脚本 `out/tasks/wideband-detection/M4/local-builds/build-fusion.cmd`。

覆盖：不同 nominal parents 的 ±8 ms 搜索测到重叠事件后合并且保留双父 lineage；超过 2^53 的正一采样点 gap；端点相接；频率接触/分离；low-IoU containment 的中心限值与两 bin Hz 换算；会改变最终分组的最佳配对 tie；按 earliest start 排序；重复至稳定；物理输出顺序与最大不确定度；内部已合并后的累计比较预算/取消原子性；工作 bytes/input budget；ID/finite/坐标系/时间质量校验；显式空发布范围。

本次仅补该 domain API、实现、测试和说明，没有改原 M4 refinement v2、CMake、infrastructure、UI，没有提交或进入 M5。项目挂接、真实数据参考对照、持久化 lineage/window union 和完整 CTest 由主 Agent 验证。
