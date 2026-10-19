# M4 最终代表性链路诊断

按用户当前要求，只选两个冻结生成文件：T00-00纯噪声与T08-00短突发（两个1ms活动，标称带内CNR6dB）。选择及新代码/包身份在运行前记录于 execution-freeze-representative-r4-release.json；模型、真值、默认和短参数、预算及容差均未根据输出调节。

使用本机 D:/profiles/anaconda/python.exe 与已有 time_suite.py，分别限定scope和 --max-files 1。两个scope均明确 diagnosticPartial=true，不作为完整门槛。

| 文件 | 原生/参考进程 | 原生/参考结果数 | 原生/参考总耗时 | 保存恢复 |
|---|---|---|---|---|
| T00-00 | 均成功 | 0 / 0 | 15.68s / 10.07s | 原生exactRecovery=true |
| T08-00 | 均成功 | 0 / 0 | 5.46s / 8.33s | 原生exactRecovery=true |

本检查确认实际模型、读文件、参数配置、终态、零结果及持久化流程可正常结束。选定1ms低CNR文件没有检出，不能将流程通过称为短突发检出能力通过。正结果的Sample/ROI、确认和DDC旅程由 NATIVE_FINAL_R5.md 的实际TorchSig文件补充，领域/物理CI16夹具另验证短活动、间隔及回退语义。

原始scope报告、逐阶段launch/completion/result/log及身份归档于 representative-r4/；全部数值比较只代表所选文件，不替代未完成的L3/L4。完整矩阵将在M5后交用户手动运行，旧R3部分历史仍保留。
