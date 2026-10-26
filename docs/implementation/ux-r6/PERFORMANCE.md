# UX-R6 性能原始事实与测量边界

尚未开展加速结果对比，不预签收益。后续在P0修复后的可完成流水线上建立重复基线，再与分块/Batch/流式实现比较；原故障失败也单独保留，不能把缩短范围的完成时长与全范围失败时长相除称提速。

| 批次 | 数据/范围 | 策略与配置 | 实际结果/耗时 |
|---|---|---|---|
| R6-0 原产品重跑 | 用户WAV20MS/s，0.8s，16M样本 | FFSCN65536，0.25ms，batch2，ROI，legacy combined32MiB | Failed164/639，131293ms，证据预算错误 |
| R6-1 基础修复正例 | 同文件，仅0.02s | 同模型/FFT/hop/batch，ROI，独立head256MiB+JSON32MiB | Completed15/15，11375ms |
| R6-1 Sample正例 | 同文件，仅0.02s | 同模型/FFT/hop/batch，Sample，独立head256MiB+JSON32MiB | Completed15/15，9472ms |
| R6-1 取消回归 | 同文件原0.8s | 同原模型/FFT/hop/batch/ROI，新配额，200窗主动取消 | Cancelled200/639，179057ms；不是完整成功 |

上述不是p50/p95或纯GPU基准；headless时间包含worker初始化、CPU谱、传输、真实TensorRT、后处理/落盘，完成正例含精修。未测UI绘制可见延迟。

R6-0 GPU采样为整设备聚合，按实际运行起止过滤，初始约10.274s未覆盖；该窗口显存3035/8151MiB、利用率均13.83%/峰80%，不是worker独占归因。主进程峰值RSS53,985,280字节不含worker。原CSV及带覆盖范围的summary保留。后续需统一launcher覆盖完整运行并同时采集worker/主进程CPU/RSS与实际模型事件。

分块必须先确定LegacyExact数值/活动段基线和容忍度；吞吐测量不得靠减窗、减候选、降精度、跳过校验或用GUI GPU绘图计数代替真实推理。未运行的大型RFbench、现场RF、Ubuntu和模型泛化不能混入本轮性能结论。
