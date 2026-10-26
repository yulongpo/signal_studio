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

## R6-2 可重复基线（优化前）

2026-10-26，Release、Qt6.11.1、TensorRT10.11 FP32、RTX5060 Laptop GPU；仍是同一用户WAV及65536/0.25ms/score0.5/batch2。每次独立启动worker/model，顺序执行，OS文件缓存可能已热；没有同时运行其它benchmark或构建。`scripts/benchmark-ux-r6.ps1` 以部署目录和仅Windows系统PATH启动，原工程只读。`scripts/summarize-ux-r6-benchmarks.py` 从实际起止时间、固定物理IoU与进程/CUDA事件生成统计，不把缺失数据补成0。

| 0.02s / 每次15窗 | 完成次数 | 总时长p50 / p95 (ms) | 吞吐p50 (windows/s) | 首解码候选p50 (ms) | 频率处理就绪p50 (ms) |
|---|---:|---:|---:|---:|---:|
| ROI | 5/5 | 11592 / 11755.2 | 1.2940 | 3151 | 6969 |
| Sample（原全长算法） | 5/5 | 9489 / 9708.6 | 1.5808 | 3148 | 6939 |

分位数对5次真实完成值作线性插值，n很小；这两种策略不能互相相除称优化收益。两个“首次”都是headless内部时间，**不是首次UI可见/稳定延迟**。完整0.8s测量单独记录，不能外推短样本吞吐。

每次输出10对象/10活动段，可信生成器TXT在本范围内也有10个活动对象；固定频率IoU≥0.3、活动时间IoU≥0.5，最大基数一对一匹配。两类F1在全部10次运行中均为1；未知时间观察只参与频率指标，不制造活动段。另3个原标签对象的活动在本范围之后，裁剪不属于漏检。本结果不是13对象全范围召回或现场RF精度。

ROI各次实际CUDA stream区间合计512.90–518.68ms；Sample为510.38–516.10ms。每次物理enqueue8次，实际logical batch分布2×7+1×1。整卡nvidia-smi从启动前每200ms采样，按实际运行时段过滤：ROI各54–55点、Sample44–46点，非worker独占。worker峰值RSS ROI335,605,760–335,958,016B，Sample335,568,896–336,166,912B；主进程峰值RSS另列原始summary，不能把二者当同进程内存。合计CPU相对一个逻辑核约98%，不是整机CPU使用率。

首轮诊断的12.204s ROI：worker-start2836.9ms、模型窗读取1051.8ms、append/chunk/manifest720.4ms、checkpoint460.6ms、frequency622.3ms、time-refinement4528.2ms。源共计算1030帧，IQ读取/输入hash3396.3ms、FFT/power1370.2ms、density393.4ms；源、host和GPU区间存在嵌套，不能相加凑成总时长。数据指向重复源频谱计算及持久化读取成本，尚未实现或预签缓存、批次和分块收益。

原始launcher、measurement、progress和GPU CSV在 `out/tasks/ux-r6/diagnostics/r6-2-baseline-{roi,sample}-002/`；归档见本节点evidence。CUDA event是流上的时间区间，不等于独占kernel活跃时间；worker CPU采样在退出前，未计析构末尾工作。
