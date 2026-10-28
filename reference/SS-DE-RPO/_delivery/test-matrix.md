# SS-DE-RPO 续作验收矩阵

统一入口scripts/verify-ss-de-rpo.ps1；当前最终代码4898fa4，证据out/tasks/ss-de-rpo/N4/final-acceptance-07/。R-01～R-20全部PASS。GPU、原始IQ、合成、fixture、QRhi和软件回退按各自层级记录，单个纯逻辑PASS不代表真实硬件PASS。

| 用例 | 状态 | 证据范围 |
|---|---|---|
| R-01 | PASS | 默认 8 GiB 授权及生产 Sample 路径；原始 IQ 20 窗口 |
| R-02 | PASS | 1/2/4/8/16 GiB 下发与实际规划/执行预算边界 |
| R-03 | PASS | checked arithmetic、低 RAM/磁盘、连续分配及 cells 分离 |
| R-04 | PASS | raw-head OFF 公共请求额度与显式额度契约 |
| R-05 | PASS | 2044×196608 B 需求/不足拒绝；实际 20 窗口 raw-head ON |
| R-06 | PASS | 正式 SourcePlan 20/159/363/2044；没有复制帧 |
| R-07 | PASS | Sample/ROI/Coarse、Golden 及质量回退 |
| R-08 | PASS | 100 ms 内五次请求、一次 latch/run；原生启动反馈 |
| R-09 | PASS | 准备/初始化取消、提交前后竞争、退出与资源收尾 |
| R-10 | PASS | 损坏模型、来源改变、磁盘/工程提交失败与重试提示 |
| R-11 | PASS | 原始 IQ 159/363/2044 完整持久发布、比较和重开 |
| R-12 | PASS | durable/non-durable supplemental model evidence 分支 |
| R-13 | PASS | 接受启动清空旧 slot；失败取消不恢复；仅编辑参数保留 |
| R-14 | PASS | 坏 shard、项目提交失败、恢复和正式结果隔离 |
| R-15 | PASS | 真实 FFSCN/SCN、实际合法 profile；注入 B2→B1 同窗重试及 B1 失败/取消 |
| R-16 | PASS | 频率索引穷举 oracle、边界/包含/零 IoU、真过载原子失败 |
| R-17 | PASS | 原始大窗 + 合成 Golden/长信号；60 s 实际 Fs bounded 执行明确回退 |
| R-18 | PASS | 来源/前缀/通道/参数/资产/profile 变化的准备复用失效 |
| R-19 | PASS | 原生 4K/150% 20 次准备反馈；Paint dispatch 完成 p95 |
| R-20 | PASS | 工程、标记、窄带、PSD/STFT/瀑布完整回归；QRhi 与软件独立部署 |

当前代码Debug完整CTest一次60/60，1052.54秒；Release一次60/60，294.04秒。启动16Qt总行（14业务/数据及初始化/收尾）。新完整GUI覆盖不复用旧运行。此前失败、120秒超时及中断保持原档，不与此轮混称。

原生准备反馈各20次，Paint dispatch完成p95：Debug6.95432ms、Release54.4546ms；handler32.3385/22.6245ms，queued callback108.553/82.4116ms单独保留。Paint目标100ms通过，不包含DWM/扫描输出。实际唯一Redmi27NU，连接2/DISPLAY6、3840×2160/DPR1.5、逻辑全屏2560×1440。宽带及窄带演示5页在Debug/Release的GPU与软件独立部署均通过；推理GPU和显示GPU分别记录。

原始20/159/363/2044在4898fa4全部重跑，完成窗口等于eligible，持久发布、恢复比较、工程重开均成功，未放宽headless来源身份规则。Raw-head ON20窗口实际3932160字节。已有60秒/20MS/s专用DSP helper执行证据保留原版本：publicationEligible=false、明确coarse fallback，不计作Sample精度或现场RF成功。本轮SampleGolden与真实模型Golden独立通过。

H1/H2 FAIL及后续只读QA问题已按REVIEW_FIX_CLOSURE.md修复并补验。最终新的H3独立审查与远端事实只由提交外精确HEAD记录证明，此表不预先授予发布批准。
