# SS-DE-RPO · 子代理 H 独立审查报告模板

**结论**：`PASS | CONDITIONAL | FAIL`  
**审查 commit**：  
**独立 agent 标识和工作目录**：  
**审查时间/环境**：  
**证据范围**：真实 diff、运行日志、测试命令、模型资产（不以提交说明替代运行事实）

## 独立核查清单

| 审核项 | PASS/FAIL/UNVERIFIED | 路径/行号/运行证据 | 严重度及修复 Owner |
|---|---|---|---|
| RAM 多层预算与 Sample 精修 | | | |
| GPU VRAM、engine profile 和 Batch | | | |
| 持久磁盘额度及 raw-head ON/OFF | | | |
| UI 启动状态、幂等、取消/切换/关闭 | | | |
| 启动→首推理真实测量 | | | |
| 长时 Sample 与密集跟踪 | | | |
| 完整 publication/recovery/sourceSet | | | |
| UX-R6 结果生命周期兼容 | | | |
| Debug/Release、UI、真实 FFSCN/SCN | | | |
| 真正子代理协作、所有权、分支安全 | | | |

## P0 阻断项（逐条，未关闭不允许完整发布）

## P1 问题

## 独立重测记录、命令、日志路径

## 未验证环境及对结论的影响

## 最终建议：允许 / 暂缓 / 禁止向 `rebuild/cursor` 同步
