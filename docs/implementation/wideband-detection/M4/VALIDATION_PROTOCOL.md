# M4 验证协议：在新结果之前冻结

2026-10-14。基线提交 `6ed5ce0338cc2020b8bf6a529874e1cc07166177`（M3）。本文件与 `validation-protocol.json` 固定验证口径；`input-manifest.json` 只记录身份、标签字面量及可用性。本次审计没有运行 DSP、模型前向、评价或生成 holdout，没有导入外部业务模块。计算中间产物统一位于 `out/tasks/wideband-detection/M4/diagnostics/`。

依据为已批准的 `docs/plans/wideband-detection/04_ALGORITHMS_AND_VALIDATION.md`、`05_MILESTONES_AND_ACCEPTANCE.md`、`08_FFSCN_TEST_GUI_CAPABILITIES.md`，M0 ADR 与 `contracts/wideband-detection/profiles/ffscn-gui-iq-v1.json`。本协议只读参考 F06–F10 的数学行为；新 reference、评价器与 C++ 实现都必须在本仓库独立编写，禁止复制或运行外部 GUI 业务模块来形成产品链路。

## 1. 输入身份与证据分组

| 组 | 本次身份 / 现状 | 使用边界 |
|---|---|---|
| G | `gui-local10-m0-20261013`，10 WAV/10 TXT/10 JPG，117 载波行、594 TIME 数值对；全部重哈希与 M0 相符 | 新冻结同源时频基线；不能直接声称恢复历史 G1 的预测、标签 hash 或 F1=0.8261 |
| H | HY epoch-210 的历史 1000 文件 manifest 指向 `/fast_data/HY_60MHz_merged`；报告和权重可用，精确数据回放未恢复 | 仅历史频率结果，单列 unavailable replay，不进入 G/T 时频门槛 |
| L | 生成器 golden WAV/TXT 与 `W:/wbSig53_rfbench20m/train/baseline/0000000.{json,sigmf-meta,sigmf-data}`；重哈希与 M0 相符 | 标签导入/物理映射/联动夹具；指定 TorchSig 样本恰好 4 事件，属于 train，不能作盲测或泛化结果 |
| T | 尚未生成或选定；生成 seed 和覆盖方案先冻结，生成后在首次推理前固定完整 manifest | 独立合成 holdout 与真实 RF 采集子域分别报告，绝不混称 |
| FFSCN-17 | 与 epoch-1200 独立的仪器谱实验 | 不参与主线 TF 比较；FP16/未通过 shape 保持实验 |

G 每 WAV 为 stereo PCM16 I/Q、20 Msps、16,000,000 复样本、0.8 s；WAV 字节数为 64,000,044。此次审计确认 24 个 M0 GUI 数学/配置来源文件、epoch-1200 checkpoint、生成器 writer/golden、TorchSig 三文件以及 M3 FP32 staging/deployed engine 身份一致。epoch-1200 SHA-256 为 `acd997957dd85b7fc560897874b575d4776e80270d7e377d1de7559e513e40e0`；M3 FP32 engine 为 `d2de250b11d6bc4a3ccec6d95a232b03ba011e085698d525c3454387094bfe81`。

已有生成器目录只做数量/代表性 WAV 头盘点：`test_sig` 6 对、`test_sig2` 11 对、`HY_test_opt` 20,000 对、`HY_test_opt_60MHz` 10,000 对、旧 partial 目录 60 对。它们没有完整选择清单、独立 holdout 身份或本次全量 hash；不能从中择优取样后称盲测。现存 `hy_trainval_config.m` 使用 20260908 seed 和 train/val substream，配置的 20,000/4,000 也不证明对应新目录全部生成。当前旧测试配置含 GMSK，类别构成必须按实际 manifest 审核。

没有在批准清单中识别出带精确事件真值的真实 RF 采集 holdout。G 的生成器 WAV、现存 MATLAB 生成器资产、TorchSig 都属于合成数据。真实 RF 子域暂记 `unavailable-not-identified`；不得用“真实文件读取”代替“真实 RF 采集泛化”。

## 2. 两条独立处理链与运行前封存

G 的主比较为 standalone independent reference 与生产 C++ pipeline，从同一完整 WAV、同一未修订 TXT、同一 epoch-1200、同一参数快照分别产生最终事件，再由同一个独立评价器计分。reference 以本仓库独立数学实现、独立 IQ→STFT→model→frequency→time 链计算；不能只读取 C++ 输出 head 作为整链 reference。读取 C++ 原始 head 的逐阶段对照属于另外的定位诊断。

默认运行完整 0.8 s / 全有效带宽；不能把只处理 20 ms ROI 的分阶段检查当作全 G 结果。G 10 文件的顺序按 input-manifest sampleId 排序。主比较用 sample/default；ROI/default、coarse/default、sample/noSplit、ROI/noSplit、coarse/noSplit 均独立报告。纯窗口不精修的观察作为诊断，不替代 L4。

首次读取任何新的模型/精修结果之前，写入 execution freeze manifest：本协议及 input-manifest SHA、完整 T 文件 hash、truth hash、source/generator hash、reference/evaluator/tested C++ binary hash、profile 快照、权重/engine/runtime hash、Python/NumPy/SciPy/Torch/FFT 库与设备、有效 shape、失败文件政策及种子。当前数学口径已冻结；独立 reference/evaluator 的新可执行文件还需实现后、正式数据运行前补 hash，不能把本协议写成工具已经实现或效果已通过。更改须在看结果前新建协议版本；看结果后的调整属于新实验，原结果保留。

开发 Python 使用 `D:/profiles/anaconda/python.exe`。本次元数据为 Python 3.13.12、NumPy 2.4.4、SciPy 1.17.1、Torch 2.12.0+cu132。正式运行的实际环境需再写入 execution freeze；不得借环境漂移静默改变分位或匹配行为。

## 3. 配置与参数生效域

默认身份保持 `ffscn-gui-iq-v1` / version 1；记录本项目数字语义版本 `frequency-postprocess-v1-unique-window-max-score`。NFFT=65536、periodic Hann、hop=floor(Fs×0.001)、模型 10 帧/5 帧步进、batch=2、topK=512、score 严格 >0.7、NMS 严格 >0.3、关联 IoU≥0.3、minHits=2、maxMiss=1、频率 mergeGap=100 kHz、noiseMargin=6 dB、FP32。不放开未经验证的不足 10 实际帧 shape，不重复谱帧。

| 实验配置 | timeStrategy / split | 生效时间参数 |
|---|---|---|
| default sample | sample / true | edgeFraction=.6、minRun=.5 ms、minOn=5 ms、minOff=10 ms、smooth=1 ms、searchMargin=8 ms |
| default ROI | roi / true | 512 frequency rows × 64 time cols、minOn=5 ms、minOff=10 ms；sample edge/smooth 不生效 |
| default coarse | coarse / true | minBurst=10 ms、splitGap=20 ms；fine 参数不生效 |
| noSplit | 每种策略另跑 / false | 仍收缩首末边界；保留 fine 策略本身的 minOn、smooth；coarse 通过 splitGap=0 禁拆分，minBurst 不生效，不改成跳过精修 |
| `ffscn-gui-iq-v1-short-burst-m4-v1` | sample 或 ROI / true | minOn=.5 ms、minOff=1 ms、sample smooth=.1 ms；其余默认不变，minRun=.5 ms、margin=8 ms |

short-burst 是单独实验配置，不能覆盖产品默认 5 ms。sample/ROI fine 生效时，不先运行 coarse minBurst=10 ms 时间过滤；STFT 频率修边仍可生效。sample/ROI/coarse 是互斥时间策略，不串联两种 fine。策略缺输入、无可靠 mask、所有 run 无效或资源超限时保留粗估计，附 `fallback` / reason / method / quality；不得写 applied。未知时间仅作为 WindowObservation，不能伪造精确事件。

## 4. F06–F10 的数学行为

### sample

以检测后的基带 `fc` 与 `bw` 对源 IQ 做测量，源 Fs 与分析 Fs 分开。搜索 `[max(0,int((t0-.008)*Fs)), min(N,int((t1+.008)*Fs)))`，整数截断政策与裁剪均记录。相位用局部索引、float64 运算后 remainder 2π 再转 float32，形成 complex64 oscillator；不使用大绝对时间相位向量。FFT 低通在该**整个真实搜索 ROI**上计算，cutoff=bw/2、taper=max(bw×.25,1 Hz)，过渡带 raised cosine，属于零相位周期 FFT 滤波。不得把它换成 FIR 或分块 FFT 并声称数学等价。

功率为 float32 |filtered|²；smoothWin=max(1,int(smoothMs×Fs/1000))，左侧补 smoothWin-1 个零，采用累计和的因果 running mean。whole search ROI 的 noise=0.5 percentile、burst=median；burst≤1.3×noise 或严格 >threshold 的 mask 占比<.03 时 fallback。threshold=noise+frac×(burst-noise)。minOnSamples=max(2,int(max(.0005,minOnMs/1000)×Fs))，minOffSamples=max(2,int(minOffMs×Fs/1000))。

先删除长度 <minOnSamples 的原始连续 on-run，再依次合并 gap **严格 <minOffSamples** 的保留 run；不能先合并两个过短 run 再使其达标。输出 inclusive run [a,c] 对应源样本半开 [i0+a,i0+c+1)。noSplit 只留从首个保留 run 到最后一个保留 run 的单一 span。`coverage>=.7` 时 frac=.5 且禁拆分。每个输出、包括 fallback 均按 t_max/文件/请求 ROI 物理范围裁切；空范围不得发布假事件。

实际整块 FFT 的信号支持是完整测量 ROI，因果 smoothing 也有左侧支持，不能把报告的 1 ms hop 当成 1 ms 独立观测分辨率。必须记录真实 source read interval、FFT 长度、searchMargin、smooth window、padding、源/分析 rate、输出映射。单块或缓存预算不足时清楚 fallback；其文件和事件继续进入效果统计，不能择优删除长目标。

### ROI

频率用 searchsorted(bin centres)，时间首端 searchsorted(t0)、末端 searchsorted(t1)-1，clip 到实际 STFT 索引；crop 包括两端，transpose 为 [frequency,time]。以 float32 bilinear、`align_corners=False` resize 到 [512,64]。执行 3 次减全局 mean，之后一次 ReLU；不是三轮每轮 ReLU。positive fraction 决定 th=1-positiveCount/total，positiveMean=正像素均值；time threshold=th/2×positiveMean，freq threshold=th/4×positiveMean。严格 >threshold 后 avgPool(kernel=5,stride=1,padding=2)，再严格 >.5 为 mask。

colDt=frameSpan×dt/64。minOn=max(1,ceil(minOnMs/1000/colDt))、minOff同样计算；使用与 sample 相同的“先丢短 run、再 gap<minOff 合并”顺序。time pooled 半开列 [a,c+1) 映射为 fa=t0Idx+floor(a×frameSpan/64)、fc=t0Idx+ceil((c+1)×frameSpan/64)-1；clip。映射后 fc-fa<2（少于3实际帧）的段跳过；父块所有段均跳过时必须 fallback。frequency 同样 floor/ceil-1 映射，并以 bin centre±half-bin 表达 Hz 边缘。continuous>=.7 或 noSplit 时仅首末 span。

ROI 输出依赖 STFT frame 支持和 pooled columns；默认固定 64 列对长父块的量化可能超过 1 ms，少于3帧的 1–2 ms 事件会 fallback。不能先假定 ROI 能达到短突发 1 ms P95。sample 短专集承担主数值目标；ROI 用同阈值独立报告结果与未达能力。

### coarse、拼接与去重

coarse 取带内最强 ceil(5%×bins) 点的每帧均值，以该 temporal profile 的 20 percentile+6 dB 得到严格 >门限的 mask。gapFrames=max(1,ceil(splitGap/dt))；先合并 gap<gapFrames，再按 minBurstFrames=max(1,ceil(minBurst/dt)) 丢短段，与 fine 顺序不同。关闭拆分仍取 mask 首末边缘。少于3帧不能伪装精修成功；空有效测量保留父块及原因。

频率融合与时间拼接是不同步骤。fine 前的时间拼接 maxGap=minOff/1000；coarse 则 splitGap/1000；禁拆分则 maxGap=0。同载波条件要求频带交集>0 且（IoU≥.3 或 containment≥.3 且 centreDistance≤.5×min(widths)+2 bins）。只有 timeGap≤maxGap+1e-12 s 的 pair 才能拼接；每次按 `(max(0,gap), -max(IoU,containment), centreDistance, min(t0), i, j)` 确定最佳 pair，合并物理边缘，union windowIds，重复直到无 pair。refined 后去重固定 maxGap=0，仅重叠/接触可合并；不能再桥接正静默间隔。

唯一 windowId 的 max score 贡献该窗口均分；scoreMean 为唯一窗口贡献均值、scoreMax 为最大，hits 为 unique-window count，coverage=hits/全部 eligible 分析窗口。输出必须保留窗口集合与分母、原 GUI 重复加权均分的对照及数字语义差异；不得把窗口 coverage 叫占空比或概率。

**continuous>=.7 是参考启发式限制。** 它依赖模型命中窗，可能把仍存在真实静默间隔的高 coverage track 当成 continuous 并禁拆分。T 必须包括该反例，报告静默被跨越的时间、原因与来源支持，不把 broad span 宣称为精确持续发射；若无法保留其物理语义，该 L4 语义项记未完成，不靠手工修边通过。

## 5. 坐标、真值与数值容差

零起始 source sample 半开区间；uint64 以十进制字符串保存。全有效带宽主线 sourceFs=analysisFs，STFT origin=0、hop=floor(Fs×.001)；完整 FFT 支持/padding、model-window nominal/context、最终 requested ROI 分别保存。G NFFT/Fs 的 FFT 支持为 3.2768 ms，不能以 hop=1 ms 抹掉支持。子带、重采样和不同 shape 需要另版本与映射对照，不进入本轮默认身份。

TXT 的 FC 是生成器 [0,Fs] 轴，比较基带 centre=FC-sourceFs/2；width=BW，边界 centre±BW/2。RF=基带+声明的 tuning centre，预测与真值必须同轴。TXT 一载波行的每个 TIME 对分别为一个真值事件；本 G 无缺 TIME 行、无丢行；坏行在运行前 manifest 中显式列出，不沿用参考 parser 静默 continue。模型 modulation 保持 unknown，标签类别不是算法识别。

本 G 时间 token 全是5位小数，量化步长10 μs；每端最大舍入误差5 μs（20 Msps 下100样本）。使用冻结文字值计分，不把该不确定度加成 IoU 或隐藏误差；边缘误差同时给 raw 与量化解释。新 T 真值必须保留 sample-exact interval 和文本导出，sample-exact 为正式计分来源。writer 的 FC/BW 是整数 Hz，理想舍入各±.5 Hz、每条频边最多±.75 Hz；这与 nominal bandwidth 对实际 occupied bandwidth 的差异分开记录。

| 阶段 | 冻结容差 / 必须精确事项 |
|---|---|
| 身份、sample/frame/window mapping、数量/顺序/IDs、hits/分母、状态/原因 | 整数及离散语义精确；不以最终 IoU 容差抵消 |
| PSD density / 频率噪底 | 原 M2/M3 `maxAbs <.02 dB`（频率噪底≤.02）；不扩大 |
| 主线模型输入 normalized | `maxAbs <.002`；SCN独立对照<.02，不混用 |
| L2 hm/bw/off | `absError<=1e-4+1e-3×abs(reference)`，逐 shape/head 报超差数量与最大误差 |
| decoded frequency edges / score | 原 .05 bin / .001；候选数量、peakIndex 与顺序精确；临界门限独立列出 |
| sample complex IQ/filtered intermediate | `atol=1e-6, rtol=1e-3`；float32参考与本地CPU算术差异明确记录 |
| sample power / smoothed envelope / linear threshold | `atol=1e-7, rtol=1e-3`；mask/run 数量与最终整数端点仍单独精确核对 |
| ROI pooled dB | ≤.02 dB；positive/time/frequency mask、runs与frame/bin映射精确 |
| final parity timing | 以相同 refined run 的 source integer sample endpoints 核对；浮点秒的唯一换算余差最多1 source sample；不容许靠1ms最终门槛吞掉段数量/映射差异 |
| evaluator math | float64 TF-IoU 与独立手算误差≤1e-12；TP/FP/FN、每阈值匹配数精确 |

sample 新浮点容差在本协议中预先固定，正式 execution freeze 明确引用；若需要不同 dtype 数字契约，只能在看到新数据结果前声明新版本与原因。即使中间浮点在容差内，只要造成非临界 mask/段数/边缘/门限决策变化也不宣称数值等价；L3 效果结果与这种 parity 结果分别判定。

## 6. 固定评价器、失败政策与 L3

每个文件、每个 TF-IoU threshold∈{.3,.5,.7} 重新独立一对一分配。矩形交集为 Δfrequency×Δtime，union=predArea+truthArea-intersection；非正 union 返回0。阈值比较≥。有效 pair 的 weight=`min(nPred,nTruth)+1+IoU`，无效 pair weight=0；优先最大有效匹配数，再在同数量中最大总 IoU（与 M0 `evaluation.py` SHA `e460dd0f3b85c233bde0247df9b5634502b2dd1bc03d27677db211b1148667e9` 数学行为一致）。不能先求最大总 IoU 后再阈值筛选。该模块仅只读数学参考；评价代码在本仓库独立写。

TP=matchCount、FP=nPred-TP、FN=nTruth-TP；P/R/F1 分母为0则0。总体主数值先聚合全部文件 TP/FP/FN 再计算（micro）；每文件与分域另报。已冻结的运行失败、资源失败、worker fault、无输出文件全部按**零预测**计入，已知真值保留为 FN；失败原因与可见 partial 诊断另存，partial 不伪称 complete。零检测、零真值、纯噪声文件均保留。预先已证实无法解析的标签属于输入问题，单列并阻止相应完整门槛声明；不在看失败后加入排除清单。

G 主 L3 gate：同 profile 的 C++ TF-F1@.5 − independent reference TF-F1@.5 ≥−.01（最多下降1个百分点）。这是本次配对新基线，不能对照历史0.8261。TF .3/.7、P/R/F1、失败文件率、分层、误差都报告；不能只公布.5或丢失败文件。

按**文件**配对 bootstrap：PCG64 seed=2026101405，10,000 repeats；每次从原 n 文件等概率有放回抽 n 索引，对 reference/C++ 使用同索引，在该样本累计 counts 后重算 F1 差值；percentile [2.5,97.5] 形成95%区间。不得按事件抽样假装文件独立，也不分别抽两实现。point gate 与 CI 分开：lower CI<−.01 时，报告迁移差值不确定，不能宣称统计上证实≤1个百分点。只有10 G文件，区间与分层小样本限制必须保留。

分裂率固定为“被至少2条互不相交预测 time spans 覆盖的真值事件数 / 全部真值事件数”；合并率固定为“跨至少2个同频且时间互不相交真值、并覆盖其中正静默间隔的预测数 / 全部有效预测事件数”。重叠关联规则为 time intersection>0 且 frequency IoU≥.3；分母为0则0。两项属于诊断，不用一对一TP数替代；dense不同频目标或emitter身份无法确定时标注歧义。另报 unknown/fallback 比例、匹配后的中心/BW MAE、time IoU、TF IoU、start/end signed error、绝对误差 median/P95；未匹配事件保留为漏检而不进入 matched MAE。真值按原TXT行号与TIME序号排序、预测按frequency/time/stable ID排序，同IoU方案的具体配对使用封存的SciPy版本；TP/FP/FN必须不受合法等值方案影响。

纯噪声报告 FP/file 与 FP/(observed effective MHz×observed seconds)，同时报告零检测文件；无法运行的噪声文件必须显示 failure，不能用“零预测”宣称误报控制通过。

## 7. T 独立 holdout：先固定 seed，再生成

固定 seed=2026101404，PRNG=NumPy PCG64，禁止复用 G、L、训练/历史 val seed；建议 fileSeed=base+1000×groupIndex+fileIndex，以编号固定，不按运行顺序消耗公共状态。使用新仓库独立生成器；不复用外部业务代码。生成前固定 source hash、完整参数 grid、用途与编号；生成后首次推理前固定 WAV/IQ/sample-exact truth/TXT hash、频率轴、seed、类/名义BW/测量CNR定义、坏文件清单。反复挑 seed 或更改真值以提高分数均为新实验。

以下为最小有意义覆盖建议（96文件）。若资源促使另选规模，必须先记录新协议版本与完整 grid，之后才生成/读取效果，不能以该建议冒称已完成96文件测试。全域默认20 Msps、full-effective-bandwidth；10 MHz/60 MHz 可作为附加域，但不能混进默认域主值。短文件须≥10个真实时间槽。

| groupIndex / 组 | 文件数 | 冻结覆盖建议 |
|---|---:|---|
| 0 / pure-noise | 8 | .2 s；4 white、4 colored complex noise，无事件；保留全部零检出 |
| 1 / low-CNR | 8 | .2 s；CNR∈{−5,0,3,6}dB × BW∈{100k,1M}Hz；真实在带/活动期CNR定义写 manifest |
| 2 / extreme-narrow | 8 | .2 s；BW∈{2,8,32,512} bins × 2个中心；bin=20MHz/65536；频带参数仍注明 nominal |
| 3 / extreme-wide | 8 | .2 s；BW∈{.10,.25,.40,.75}×Fs × 2个中心，完整位于可用频带 |
| 4 / dense-overlap | 8 | .2 s；载波数∈{4,16,64,128} ×2布局，包含邻近、部分重叠、同中心宽窄嵌套；保留topK截断事实 |
| 5 / band-edges | 8 | .2 s；100kHz band分别靠左/右边，guard∈{0,1,4,16} bins；频边不越Nyquist |
| 6 / same-frequency-gaps | 8 | .2 s；静默gap∈{.5,1,10,20}ms × 低/高活动比例，特设模型coverage≥.7反例；另做minOff等值±1sample golden |
| 7 / long-duration | 8 | Fs20M；duration∈{1,2,4,8}s ×2配置；包含跨cache/任务块、近连续及真间隔；overbudget fallback也计入 |
| 8 / short-1-to-8ms | 32 | duration∈{1,2,3,4,5,6,7,8}ms × CNR∈{6,20}dB × onset phase∈{0,.5hop}；每文件≥40ms，2个同频事件、gap按fileIndex奇偶为1/3ms；至少64事件 |

所有生成真值使用原始 sample 半开边界；以小批独立检查确认标签与 IQ 活动支持一致，不能用算法检测结果反生成标签。只有合成边界支持完整的样本才能承担≤1ms目标；实际 occupied-band 误差、波形成型/filter tails与 nominal 时间定义应披露。

另外准备不依赖模型输出的行为 golden：minOn/minOff 等值及±1样本，先删短run再合并，coverage=.7等值，zero/constant/non-finite、negative/empty crop、EOF/t_max/ROI、同频正gap去重反例、少于3帧ROI fallback、pooled半开映射、cancel与预算失败。它们证明数学/状态边界，不替代 T 模型效果。

真实 RF holdout需另有独立来源、capture配置、IQ hash、人工/仪器标注来源及不确定度、sample范围、授权可用性。当前缺失不阻止合成测试，但“真实 RF 泛化”验收项保持未完成。

## 8. L4 与报告输出

L4 数学/语义：sample、ROI、coarse、各 noSplit 组合必须可以独立复现；两种 fine 成功/fallback/预算/取消的原因、原始/精修边界、真实支持、有效参数、unique-window证据都存在。接受/人工修订/DDC联动不能改原模型真值或原始事件。M4 的原生复核/通道旅程由主实施另外验证；此协议不提前宣布其通过。

short 专集以 `short-burst-m4-v1/sample` 为首轮主目标：事件 Recall@TF-IoU .5≥.90；匹配事件的 start/end 绝对误差合并 edge P95≤1ms，同时单列 start/end P95、未匹配率、默认5ms结果、CNR与1–8ms各时长。quantile 使用 NumPy linear 方法。ROI/short 用同指标/同阈值单独报告，不能与 sample混合获得主值；未达时明确ROI短能力未完成。默认5ms或粗10ms失败不能被短配置成绩隐去。窗口覆盖高导致静默跨越、fallback broad span和漏检仍参与指标，不允许人工修边抵消。

最终诊断至少包含：freeze manifest、每文件输入/真值/预测数量与终态、逐阈值匹配清单、两实现差值与bootstrap、domain/group/profile/strategy配置、初始/修边/活动段与fallback证据、失败样例（包含所有失败的索引）、原始head/支持范围与SHA、资源/时间实测及限制。生成器、独立 reference、C++ pipeline、评价器身份分开保存。性能不以合成UI或非空结果替代。

退出结论分列 L1/L2 数值 parity、G/T L3、L4 sample/ROI/coarse/short、真实RF可用性、M4产品旅程。不能将 H 频率历史、FFSCN-17实验、L导入成功或旧M3频率检查写成完整M4二维效果。协议与输入审计目前已完成；新效果、holdout生成、原生M4验收尚未执行。
