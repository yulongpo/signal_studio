# UX-R6 连续执行状态

| 阶段 | 当前证据/状态 |
|---|---|
| R6-0 | 完成：fetch并核对ec511662；原工程两份164/639失败manifest；本轮用户原IQ实际重跑相同失败；BUG02真实Qt/服务/保存重开诊断；Release测量入口构建通过 |
| R6-1 | 基础修复与定向自检完成：D/R完整构建、各11核心+3生命周期/UX组通过；原IQ ROI/Sample短范围各15/15；原完整请求跨旧断点至200/639后主动取消。完整639同参及所有性能/质量终验仍待R6-7，不计成全量通过 |
| R6-2 | profiling和重复短基线完成：ROI/Sample各5次完成；Debug5组、Release先4后3组通过。完整原请求已处理639/639窗，但ROI精修23/44因第二处容量不一致Failed；证据归档，未计成功。后续R6-3新构建Task测试已补测Release terminal异常保护 |
| R6-3 | 定向闭合，不是全矩阵或 p50/p95。oneMKL 固定 1 线程并只从可执行目录加载 `mkl_rt.3.dll`；探针 `SS_MKL_DLL=off` 仍用手写变换。Release 样本流/恢复/时间管线/能量/来源测试通过。原 WAV 0.02s 显式 FIR 新跑 15/15、活动 F1=1；此前 F1=0.75 保留。完整 639 窗 ROI 新跑 Completed、639/639、精修 44/44，1965150ms，活动 F1=0.871；1731s 失败运行不作加速分母。完整质量与 p50/p95 仍属 R6-7 |
| R6-4 | 定向完成。FFSCN profile 只含 batch 1 和 2。0.02s ROI：batch1 为 15 次 enqueue、15781ms；batch2 为 7 次 batch2 加 1 次 batch1、14510ms。4/8/16 降到物理 batch 2，引擎未重建。用户 batch 16 先受 32MiB host payload 限到 worker limit 8，再降到 2。首次 4/8/16 被 JSON 上限 2 拒绝，放宽到 16 后才进入降级。SCN 物理 batch 仍为 1，本节点未另跑 SCN 计时。默认档保持 2。显存是整卡 nvidia-smi，不是 worker 独占，也不含这次 headless 运行的 Qt 显示 GPU |
| R6-5 | 定向完成，无 Qt。预览事件有稳定 trackId、递增 revision，以及 Created/Updated/Stable/Closed/Merged/Finalized。队列超限丢最旧并计数，不截断正式轨迹。未精修对象 timeLocalized=false。取消封存为 Closed，成功为 Finalized。同一信号跨窗更新同一 ID；无现有合并规则时接近的两条轨迹不合并。Release `signal_studio_detection_postprocess` 14 组通过。预览不写入 file.marks |
| R6-6 | 预览进入检测结果状态行和时间图：只在算法来源层绘制，标签为“预览 · 时间未定位”，不进入命中选择、人工转换或 DDC。轮询约 10 Hz，且不因此重算 IQ 频谱。2026-10-27 Release 全屏烟测在 Redmi 27 NU、连接 2、DISPLAY6、3840×2160、DPR 1.5、逻辑 2560×1440：GPU 为 QRhi D3D11 / AMD Radeon 880M，主图 textureUploads=1，pass=true；软件回退为 QPainter，hardwareRenderer=false，pass=true。这是显示 GPU，不是 RTX 推理。这次烟测没有跑出一条活动预览框 |
| R6-7 | 待实施：D/R回归、多场景性能/质量/取消/恢复、截图/独立启动 |
| R6-8 | 未审核、未推送；最终HEAD须由未参与实现的独立Review明确APPROVED |

任何阶段实际失败、跳过和未验证边界独立记录，不把上一版本测试说成本轮新执行。reference任务包原文是范围依据，不能按现有完成量缩小成功条件。
