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
| R6-7 | Debug/Release 各 19/19：检测、存储、任务、来源、样本流和 UX-R5 注释组通过，与 R6-3 的定向通过分开。Release UI 在补上系统字体后 geometry 2 失败、parameters 2 失败、annotations 通过；无字体的首轮不计入。Debug UI 未跑。干净的 0.02s n=3：ROI p50/p95 13960/14054.5 ms，活动 F1=1；显式 FIR p50/p95 16823/17120 ms，活动 F1=1。完整 639 窗仍是 R6-3 的 n=1。取消 1 次为 Cancelled、6 窗、8199 ms。合成样本流含低 SNR、稀疏突发和全零，不是现场 RF。大型 RFbench 未跑 |
| R6-8 | 未审核、未推送；最终HEAD须由未参与实现的独立Review明确APPROVED |

任何阶段实际失败、跳过和未验证边界独立记录，不把上一版本测试说成本轮新执行。reference任务包原文是范围依据，不能按现有完成量缩小成功条件。

## 2026-10-28 旅程收尾定向复验

这是一次 Debug 定向复验，不是全量 CTest，也不是 R6-7 的 UI geometry/parameters 复跑。构建入口 `scripts/build.ps1 -Configuration Debug`，实际 preset 为 `local-debug`（继承 `vs2026-qt611-debug`）。CTest 8/8 通过，总墙钟 187.51 s：

| CTest | 结果 | 耗时 |
|---|---|---|
| signal_studio_detection_time_pipeline | 通过 | 14.86 s |
| signal_studio_detection_frequency_analysis | 通过 | 0.29 s |
| signal_studio_detection_store | 通过 | 1.82 s |
| signal_studio_detection_task | 通过 | 45.28 s |
| signal_studio_detection_source | 通过 | 8.68 s |
| signal_studio_ux_r5_detection | 通过 | 22.91 s |
| signal_studio_ux_detection | 通过 | 90.49 s |
| signal_studio_ux_r6_preview | 通过 | 3.14 s |

`signal_studio_ux_detection` 含已改的 `uxD09_intermediatePreviewAndRedetectClear`，同组其余检测 UI 用例这次也通过。它不是 `signal_studio_ui` / `signal_studio_ui_parameters`。R6-7 已记录的 Release UI geometry 2 项、parameters 2 项没有重跑，也没有改成通过。主 UI 的 Debug geometry/parameters/annotations 这次仍未跑。

639 窗原请求复跑没有完成。配置是 Debug，因为这次要跑进检查点追加校验后的二进制；R6-3 那次 Completed 1965150 ms 是 Release，不能把两次当成同一配置的加速比。命令为 `scripts/benchmark-ux-r6.ps1`，Repeat 1，ExpectedLifecycle Completed，TimeoutSeconds 7200，strategy roi，batch 2，raw-head 256 MiB，dsp gpu。请求 JSON `out/tasks/ux-r6/diagnostics/run-0c82d126-38e7-4b14-8778-491fc5c670f3-original-manifest.json`，SHA-256 `94525B9B66B3B11ACAE3B360DF34A3C8F04917163AD000C5C7BAB6192E995CAB`，与 `docs/implementation/ux-r6/evidence/baseline/` 下同名文件一致。原 IQ `D:/coding/FFSCN_train2/FFSCN_TEST_GUI/test_data/data/20260728-045101-650_bw_20000000_sampleTime_0.8_rollOff_0.35.wav`，64,000,044 字节，SHA-256 `A95A415E945A026E10079462733A2E47FDC861C4EC84E44A17E031BD9B0BCE3D`。Engine 为请求里的 `out/vs2026-qt611-release_bin/detection-models/ffscn-gui-iq-v1/ffscn_epoch1200_dynamic_fp32.engine`，23,722,236 字节，SHA-256 `4E14E422E7966EB6F554E322C2D390534650E1B8DD0CEE6E04F2ED4305CEFC03`，与 manifest 的 `modelSha256` 一致。标签文件沿用 `docs/implementation/ux-r6/evidence/fixtures/20260728-045101-650_bw_20000000_sampleTime_0.8_rollOff_0.35.txt`。输出目录 `out/tasks/ux-r6/diagnostics/r6-t5-639-window-01/`。

启动 UTC `2026-10-27T16:47:40.5905758Z`。progress 在 benchmark elapsed 972283 ms 进入 `publishing`，此时 639/639 窗、精修 44/44。此后进程不再消耗 CPU。约 10 分钟后进程被停止，启动器记录结束 UTC `2026-10-27T17:14:23.9246803Z`，observedLifecycle 为 Missing，pass 为 false，hardTimeout 为 false。没有写出终态 measurement，因此这不是 Completed，也不是模型返回的 Failed/Rejected。不能把它或约 1731 s 的失败运行当作加速分母。唯一完整 639 窗 Completed 仍是 R6-3 的 Release n=1、1965150 ms。这次 headless 运行的 nvidia-smi 采样是推理机器上的整卡记录，不是 QRhi 显示 GPU。

根因：这次挂起不在 `DetectionPanel::stillCurrent` 的 `BlockingQueuedConnection`（`SignalStudioDetectionBenchmark` 不构造面板，也不做正式替换）；producer 在 `DetectionTaskManager::finish` 已把 manifest 写成 lifecycle=Completed、phase=completed（2026-10-27T17:04:06.8845480Z，639/639、精修 44/44，最终 shard 含 `frequency-publication-end-v1`）并置 `finished` 后，`execute` 退出时 `previewGuard` 再抢 `mutex_`，主线程 `DetectionTaskManager::pump` 却持同一把锁 `thread_.join()`，两边零 CPU 互等，内存进度停在 publishing，终态 measurement 未写出，本次仍未完成。

## 2026-10-28 pump/join 终态锁修复与 Debug 639 窗复跑

`DetectionTaskManager::pump` 在确认 producer 已 `finished` 后，先放开 `mutex_`，再 `thread_.join()`，然后重新持锁收尾并启动下一任务。`finish()` 仍在锁内写入终态 run 并置 `finished`。`execute` 退出时的 `previewGuard` 仍在锁内封存预览。取消、代次和预览快照继续在锁内读写。`stillCurrent` 与正式结果替换语义未改。

定向测试已通过。命令 `scripts/build.ps1 -Action Test -Configuration Debug -TestRegex '^signal_studio_detection_task$' -VerboseTests`，Debug，1/1 通过，42.76 s。其中 `testPumpJoinLetsProducerExposeTerminalSnapshot` 用条件变量把 producer 停在 `finish()` 之后、终态预览加锁之前；调用线程进入 `pump()` 的 join 段后 producer 才能继续取 `mutex_` 并退出，随后同一调用线程的 `snapshots()` 读到 lifecycle=Completed、phase=completed。5 秒只是死锁失败界，通过路径不等待这段时间。

639 窗复跑已完成。Debug 二进制为上述修复之后重新链接的 `SignalStudioDetectionBenchmark`。命令 `scripts/benchmark-ux-r6.ps1`，Repeat 1，ExpectedLifecycle Completed，TimeoutSeconds 7200，strategy roi，batch 2，raw-head 256 MiB，dsp gpu。请求 JSON 与标签文件与上一节挂起运行相同。输出目录 `out/tasks/ux-r6/diagnostics/r6-debug-639-pump-join-01/`。启动 UTC `2026-10-27T17:39:56.7567493Z`，结束 UTC `2026-10-27T17:54:40.6421865Z`。measurement `pass=true`，lifecycle=Completed，phase=completed，窗口 639/639，精修 44/44，elapsedMs=883035，exit 0，hardTimeout=false。progress 在 870163 ms 进入 publishing（639/639、精修 44/44），在 883034 ms 进入 completed。nvidia-smi 仍是推理机器整卡采样，不是 QRhi 显示 GPU。

这次是 Debug n=1 的全程 Completed，耗时 883035 ms。R6-3 的 Release n=1、1965150 ms 仍是另一配置的全程记录。约 1731 s 的失败运行和上一节 Missing 运行不作为加速分母。本次不计算 p50/p95。R6-7 的完整质量矩阵，以及 Debug/Release UI geometry、parameters 的未通过或未跑项，都保持原记录。
