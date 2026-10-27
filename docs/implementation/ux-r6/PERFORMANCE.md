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

## 完整0.8s的失败定位数据

`r6-2-baseline-roi-full`实际处理639/639模型窗、320次物理enqueue，在ROI精修23/44候选因谱cell容量不一致失败；总1730925ms。GPU stream25181.8ms，读取模型窗47044.7ms、frequency24236.7ms、time-refinement108181.1ms；append-chunk/manifest981598.4ms和checkpoint490231.3ms合计约85%的总时长，说明重复校验历史前缀是优先处理的成本。具体阶段包含验证、JSON处理和CPU hash，不能把它全部称磁盘等待。没有以提高GPU batch就能解决整体耗时的证据。

该长运行与后半段定向构建/测试有并发，且终态Failed，只作根因/资源归因与完整输入覆盖证据；不加入任何Completed p50/p95，也不作为提速分母。原始终态、进度、GPU采样及并发说明均保留。后续修正需以完整新运行实际闭合，前缀观察不可当成功结果。


## R6-3 CPU 对照与实际采样率资源记录（GPU 接入之前）

以下为本轮真实执行；数据来自 stateless 合成 IQ callback，不包含模型推理、真实 WAV 文件读取、Coarse 回退或 UI。原始报告、进程资源采样和 launcher 已归档至 `evidence/r6-3/r6-3-cpu-*`。CPU 既有算法保留为数值及性能对照；按用户 2026-10-27 新指令，后续主路径优先使用成熟 GPU SDK，不继续扩展自写 FFT/归约/排序。

20MS/s、0.1s bursts、每配置独立进程 n=3：LegacyExact 为1156/1147/1185ms，p50=1156ms、线性插值p95=1182.1ms，峰值RSS最大37,257,216B；CPU FIR v2 为576/580/553ms，p50=576ms、p95=579.6ms，峰值RSS最大18,984,960B。两模式活动区间指标F1=1。此前 generic FFT 的单次FIR2903ms、Legacy1146ms同样保留，不能把弃用实现的失败测量丢弃。

| 实际20MS/s长度 | continuous FIR(ms) | bursts FIR(ms) | 结论 |
|---|---:|---:|---|
| 1s / 20M样本 | 4655 | 4887 | 各n=1 |
| 10s / 200M样本 | 47354 | 48587 | 各n=1 |
| 60s / 1.2B样本 | 276945 | 291941 | 各n=1；scratch约9.6GB；峰值RSS不超过19,025,920B |

bursts 各长度活动F1=1且通过局部精度门禁；continuous 各长度触发原有 burst/noise ratio 回退，`publicationEligible=false`，不能计为连续活动定位成功。该表仅证明实际Fs下有界执行/清理；Legacy在96MiB grant下1/10/60s均明确ResourceLimited（raw pass=false），没有完成耗时，不作为加速比分母。OS文件缓存不计入进程RSS，资源CSV保留VAS采样；不是现场RF或GPU证据。

原 WAV .02s Auto 完成15/15、10对象/10活动段，几何与R6-2冻结Legacy一致。第一次显式FIR因缺少完整Legacy精度重试，仅6活动段、事件F1=.75，是质量失败，保留在 `evidence/r6-3/r6-3-real-sample-fir-short`。

## R6-3 定向补测（2026-10-27）

oneMKL 发布路径固定 `MKL_NUM_THREADS=1` 且 `MKL_DYNAMIC=FALSE`，并调用 `MKL_Set_Threading_Layer(INTEL)`、`MKL_Set_Num_Threads(1)`。`mklDetail` 含 `threads=1`。运行时只加载可执行目录中的 `mkl_rt.3.dll` 及同目录预加载的 DFT 依赖，不搜索当前目录。`SS_MKL_DLL` 仅作探针：绝对路径，或 `off` 强制手写变换。65536×10 短窗相对手写约 1.14 倍，不记成流水线加速。

首轮把 `mkl_set_dynamic` 放在线程层装载前，Release 样本流/能量/时间管线/来源测试在 `MKL_SET_DYNAMIC` 访问违例。改为先预加载同目录依赖再选择 Intel 线程层后，同一组测试通过：`signal_studio_detection_sample_energy`、`sample_stream`、`sample_recovery`、`time_pipeline`，以及来源测试中的密度差 0、`threads=1`、部署目录加载和 `off` 回退。

原 WAV 0.02s 显式 FIR（`stream`）新运行：Completed 15/15，13996ms，10 对象/10 活动段，活动 F1=1，频率 F1=1。证据 `evidence/r6-3/r6-3-real-sample-fir-short-02/`。启动器因重定向前的 `WaitForExit(int)` 没读到进程退出码而把 pass 写成 false；`measurement.pass` 为 true。旧 F1=0.75 记录不删。

完整 639 窗 ROI、同一原请求和 256MiB head 配额：Completed 639/639，精修 44/44，总 1965150ms，44 对象/44 活动段。活动事件 F1=0.871（tp 37 / fp 7 / fn 4，真值 41）；频率-only F1=0.456（tp 13 / fp 31 / fn 0，真值 13）。物理 enqueue 320 次，CUDA stream 区间合计约 27453ms。证据 `evidence/r6-3/r6-3-full-roi-completion-01/`。谱 cell 容量错误没有再现。这是一次新的完成运行，不用 1730925ms 的失败运行当加速分母，也不把短窗 1.14 倍写成端到端加速。n=1。启动器退出码同样未被旧脚本记下。

## R6-4 真实 Engine batch（2026-10-27，Release，RTX 5060 Laptop）

同一原 WAV、0.02s、ROI、FFSCN 65536/0.25ms。每个档位一次，n=1。profile 支持的档位是 1 和 2。显示用的 Direct3D 不在这次 headless 推理里；nvidia-smi 是整卡采样。

| 请求 | 实际物理 batch | enqueue | 总时长 ms | 首候选 ms | 整卡 memory.used 峰 MiB | 说明 |
|---|---:|---|---:|---:|---:|---|
| 1 | 1 | 15×1 | 15781 | 5143 | 2600 | profile 含 1 |
| 2 | 2 | 7×2 + 1×1 | 14510 | 5303 | 3046 | profile 最大档 |
| 4 | 2 | 7×2 + 1×1 | 14770 | 5272 | 3046 | 降到 2，未重建 Engine |
| 8 | 2 | 7×2 + 1×1 | 14580 | 5306 | 3046 | 降到 2 |
| 16 | 2 | 7×2 + 1×1 | 15259 | 见 16b | 3044 | 用户 16；32MiB payload 先把 worker limit 降到 8，Engine 再选物理 batch 2 |

4/8/16 的第一次尝试在约 12ms 被 `batchSize is outside its integer budget` 拒绝，因为解码上限仍是 2。上限改为 16 后，请求能到达 Engine，并写下降级原因。做不到物理 batch 16。SCN 代码路径仍是物理 batch 1、多次 enqueue 分开计数；这次没有另开 SCN 计时。默认档保持 2。原始 measurement 在 `out/tasks/ux-r6/diagnostics/r6-4-tier-*` 与 `r6-4-tierb-*`。
