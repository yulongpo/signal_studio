# M4 终态写入失败的阶段补审

预退出 QUALITY 发现：准备 Completed 终态时 computationPhase 已设为 completed；若终态持久化失败，lifecycle 改为 Failed，但阶段仍可能显示“已完成”。这与实际发布失败和禁止自动发布的行为不一致。

修正仅在终态写入拒绝且阶段为 completed 时，恢复为 publishing（结果落盘 / 发布）。其余失败阶段保持实际记录；覆盖、原始错误、有界 UTF-8 摘要、来源校验、恢复前缀及禁止自动发布策略不变，也没有新增重试。

变更保存为 deferred/io-diagnostics-r2 四文件快照，仅 task 实现与既有 terminal 拒写断言相对上一版变化。原来 Interrupted/recoverable 前缀、原始分片及禁止自动发布检查保持。逐文件身份见 io-diagnostics-r2-draft-identity.json；四文件与隔离候选一致。

SPEC 源码复核通过。候选自身 scripts/build.ps1 Debug Build 通过，相关 signal_studio_detection_task CTest 1/1 通过（13.83秒），包含既有全部 task 行为组，见 terminal-phase-focused-ctest-r2.txt。R3 输入与正式失败历史未变；该结果不能扩写为新的完整 UI、原生或主目录集成验收。最后质量复核及节点退出仍待最终证据。
