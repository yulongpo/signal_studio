# R6 文档与交付自检

日期：2026-10-15。前置R5已自检、复审并提交`b5187e0`后开始。分支仍为`rebuild/a143-native`；R6没有修改C++产品或重建可执行程序。

README已按实际能力重写：检测与三来源记录已实现，工程版本为5、兼容1–4；主FFSCN/SCN均11层、最小模型宽度2048、全部统计谱可检测，分析参数与部署profile分开。保留M0-P及用户例外、GPU显示/推理/CPU DSP区别，公开模型不随Git及当前包兼容边界，明确Debug反馈未达建议目标、L3/L4/真实RF等待验证项。完整部署顺序、当前模型SHA及手动入口见同目录文档。

旧`freeze_validation.py`只能更新模板已有文件，可能漏记新动态engine或新增源码。本节点增加显式`--include-current-inputs`：保留旧不可变资产校验和失败历史，纳入全部跟踪构建输入（含resources）及两个实际部署目录；记录当前HEAD/工作树、模板身份与适配版本。默认旧调用保持兼容，自检临时目录迁入R6。实际模板的`dataAssetsVerifiedBeforeInference`以及旧`datasetFiles`均校验，不更新数据hash；声明数量不符、数据缺失或变化均拒绝。它只建立字节身份，不代替推理或效果评价。

| 检查 | 实际结果 | 证据 |
|---|---|---|
| 身份工具CPU自检 | PASS，旧freeze覆盖/不可变资产变化均拒绝；新增runtime/资源身份纳入且去重；实际数据键的变化/缺失及旧数据键变化均拒绝 | `freeze-self-check.txt` |
| 既有套件CPU自检 | PASS，9 scopes/220对计划、失败/恢复/中断保护及CPU子进程超时 | `suite-self-check.txt` |
| 4个工具CLI帮助 | PASS，未执行打包/导出/模型 | `*-help.txt` |
| Python语法、Black80、Ruff | PASS | 源码AST、Black check及Ruff实际退出0 |
| 文档本地链接/PowerShell | 所有本地链接存在，代码块全部解析；没有执行文档中的模型/批量命令 | `document-check.json` |
| R5产品输入身份 | 13/13 SHA不变 | `document-check.json`，R5原始快照 |
| 空白与最终变更范围 | diff检查通过；仅文档、约定和CPU身份工具 | 本节点Git差异 |

本次没有启动新的批量推理、Qt回归或GPU测试。R5已通过完整Debug/Release和原生/部署验证；R6修改不进入C++产品，相关CPU工具已自检，无需重复完整产品回归。R5部署身份仍适用，README没有将内部构建revision说成最终文档HEAD。

只读复审发现并关闭三个P2：实际数据清单键漏检、编译resources漏记、最终截图示例缺少`-ProductionModel`。增强后的CPU自检使用明确的合成字节夹具验证数据变化/缺失拒绝，并实际枚举/记录当前构建输入和部署身份；不是重新验证308项真实G/T数据，也没有执行文档中的批量命令。旧自检输出保留在R6任务目录，最终归档采用补强后的结果。

最终只读复审PASS，三个P2全部关闭，无剩余P1/P2。节点完成后先本地提交R6，再按用户既有授权统一推送当前分支；不新建PR、不合并其它分支。推送结果在执行后核对远端HEAD/ahead-behind并向用户报告，不在这里提前伪造网络成功。
