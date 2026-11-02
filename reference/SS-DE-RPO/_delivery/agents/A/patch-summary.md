# 补丁摘要（Agent A）

分支 `fix/ss-de-rpo-a`。基线 `82935ab34ec6126707da152bf46d1f352458ab0c`。未推送。

## 改了什么

- `domain/detection.h` / `domain/detection.cpp`  
  增加运行时类型 `ResourceDecisionKind`、`ResourceDecision`，常量 `kSampleContiguousAllocationCapBytes`（1 GiB 连续分配上限）和 `kSampleWorkUnitCellCeiling`（256 Mi cells），以及 `checkedMulU64`、`decideSamplePhaseBudget`。  
  大于 1 GiB 的授权不会仅因偏大被拒绝。估计驻留大于连续上限但不超过授权时返回 `Adapted`，`granted` 为 1 GiB，并要求规划器分块。cell 超过工作单元上限、块驻留超过授权、授权为 0，分别使用独立错误码。
- `ui/detection/detection_resource_options.cpp`  
  增加只读标签 `workingSetAuthorizationNote`，显示物理内存（`detectionPhysicalMemoryBytes`）、默认工作集，以及一句「更大的工作集授权是使用上限，不是一次连续分配。」不参与滚轮改值和草稿提交。
- `reference/SS-DE-RPO/_delivery/agents/A/`  
  本目录的清单、契约、接口请求、状态与本摘要。

## 没改什么

- `validateDetectionRequest` 的条件与文案。
- `DetectionRequest` 字段、工程 JSON、QSettings 键、`applyTo` 写入的数值。
- Sample stream、时间精修、`detection_task`、检测卡片/面板、模型后端、结果服务、CMake、`M1_CONTRACT.md`。
- 8 MiB shard、batch 档 `{1,2,4,8,16}`、FFSCN/SCN 网格。

## 为何这样分

默认 8 GiB 工作集在时间精修里变成 6 GiB 授权和 512 Mi cells，再被 Sample 的 1 GiB / 256 Mi cell 检查一起拒绝。1 GiB 是单次连续分配上限，256 Mi cells 是工作单元上限。授权可以大于 1 GiB。现有拒绝点仍在 B 的文件里，B 拥有行为修复。
