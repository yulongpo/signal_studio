# UX-R6 实际验证记录（持续更新）

本轮产物和首次失败均保留 `out/tasks/ux-r6/`。范围依据为reference四文件，不因局部测试绿灯缩小最终目标。当前仍未终审、未推送。

## R6-0 原故障

用户原工程、两份164/639错误manifest、本轮原WAV/原冻结请求失败重跑、GPU采样及BUG02实际Qt诊断见 `evidence/baseline/`。详细根因见 ROOT_CAUSE。原失败中的ROI与用户描述Sample须分开；没有猜测OOM。

## R6-1 版本与构建

构建输入为本仓库 `cf1584f` 后的R6-1工作树，随后本节点提交冻结相同实现；不能把程序内嵌的旧buildRevision当作整个工作树身份。Debug/Release 公共完整构建分别退出0，原始日志 `evidence/r6-1/{debug,release}-build.txt`。本轮没有更换Qt、模型权重、Engine、精度或DSP算法。

| 实际批次 / 每配置 | Debug | Release | 范围 |
|---|---:|---:|---|
| 主代理核心/存储11组 | 11/11，94.54s | 11/11，28.52s | store/task/protocol/source/model_backend/pipeline、core/project_file/annotations/annotation_store、R5 compat |
| BUG02子任务3组 | 3/3，54.18s | 3/3，17.53s | 生命周期11业务case、R5 UI4业务函数、Layout8业务函数，均新构建实际执行 |

两批并行调度，耗时不能相加解释为单次端到端性能。此处测试无失败，日志保留实际Qt totals及跳过统计；它们不证明实屏、真实模型质量或全R6矩阵。

新增/强化边界：独立raw-head配额与旧combined兼容、原子超额拒绝、完整raw内容篡改；整任务配额不足不得启动worker；非空旧committed在参数/模型/运行/异常与重开保持ID/证据/可见性；pending/retired不穿透；成功含0仍整体切换；历史转换、人工权限、通道及DDC/事务/GC既有断言保留。

## R6-1 用户原WAV真实FFSCN

同一20MS/s、int16 IQ、16M样本WAV及原65536/0.25ms/score0.5/batch2模型参数；新请求显式选择256MiB独立raw-head配额，JSON配额保持32MiB。原输出/进度报告为 `evidence/r6-1/` 对应子目录，完整head资产仍在out。

| 运行 | 目标与实际 | 原始状态 | 用途与限制 |
|---|---|---|---|
| ROI，范围限制0.02s | 15/15；11375ms；10对象/10段 | Completed，pass=true | 完成/精修/落盘正例；不是完整0.8s |
| Sample，范围限制0.02s | 15/15；9472ms；10对象/10段 | Completed，pass=true | 独立Sample分支正例；不是完整0.8s |
| 原完整范围、200窗时取消 | 200/639；179057ms；23前缀观察/0活动段 | Cancelled，pass=false，cancellationSent=true | 跨旧164失败点及实际取消；不能称639/639通过或正式发布 |

该取消试验的预期行为断言成立，但未改写原始CLI pass=false。旧正式结果保护由真实服务/.ssproj/Qt回归证明，headless benchmark不会改原用户工程；前缀观察不是正式结果。

已找到原WAV同名生成器TXT（13对象），频率坐标继续采用现有可信解析器 `generator FC - Fs/2 + bound Fc`，后续质量测量使用该依据，不能按预测结果反推标签。尚未在本节给出完整质量指标。

## 尚未闭合

完整原范围639窗口同参完成；批次1/2/4/8/16真实能力与吞吐/降级；分阶段计时及同设备p50/p95；0.1/1/10/60s分块数值/性能；多场景检出质量与轨迹；有界临时预览/增量UI；真实4K150 GPU/软件、最终回归、独立审核及安全发布均仍待后续节点。大型RFbench按要求不执行。
