# SS-DE-RPO · Agent G 性能对照

没有测量。下表没有填入任何毫秒、字节峰值或 batch。空着表示未测，不是零。

| 指标 | 基线 82935ab | 本分支 |
|---|---|---|
| 冷启动 T_click→T_first_infer ×3 | 未测 | 未测 |
| 热启动 T_click→T_first_infer ×3 | 未测 | 未测 |
| 首候选 | 未测 | 未测 |
| 全推理 | 未测 | 未测 |
| 频率处理 | 未测 | 未测 |
| Sample 精修 | 未测 | 未测 |
| 持久发布 | 未测 | 未测 |
| 总时长 | 未测 | 未测 |
| CPU / RAM / VRAM / Disk 峰值 | 未测 | 未测 |
| 最后有效 batch | 未测 | 未测 |
| R-08 `QElapsedTimer` | 未启动 | 未启动 |
| R-19 p95 | 未测 | 未测 |

UI 文件在 `BLOCKED_UNTIL_AGENT_C` 下直接返回，没有构造 `DetectionCard`，因此没有点击耗时。契约要求报告实测 p95，禁止把未测的 p95 < 100 ms 当成通过。本文件不给出 p95。

没有资源统计 JSON：没有进程可采样。Mock batch 时间没有被写成 GPU 吞吐。设备、驱动、engine、IQ 身份均未采集。
