# SS-DE-RPO 续作开发与验收交付

开发基线为 `7220b2e`，最终实现代码为 `75cf063`（含H1审查后的修复），分支 `rebuild/cursor`。资源治理、Sample、准备阶段取消、跟踪索引、正式发布恢复、准备复用和真实 GPU 路径均已接入。最终审查 SHA、结论与推送结果保存在提交外；最终提交后由新的独立 H2 会话审查，只有 PASS 且远端可正常快进才推送。

可运行程序：`out/vs2026-qt611-release_bin/SignalStudio.exe`；Debug 对应 `out/vs2026-qt611-debug_bin/SignalStudio.exe`。构建使用 Qt 6.11.1 / VS 2026 x64 / C++20。Git 不包含部署模型、CUDA/TensorRT/Qt DLL 和用户原始 IQ 文件。

环境：AMD Ryzen AI 9 H 365、20 逻辑处理器、约 64 GiB RAM；TensorRT 10.11.0.33 / CUDA 12.9 / RTX 5060 Laptop SM12、8 GiB VRAM。QRhi D3D11 显示使用 AMD Radeon 880M，与 NVIDIA 模型推理分开。原生验收选择唯一 Redmi 27 NU、连接 2、DISPLAY6、3840×2160、150%，逻辑全屏 2560×1440。

原始 IQ 来自用户现有 0.8 s、20 MS/s 交替 I16 WAV。SourcePlan 实际生成 20/159/363/2044 窗口；2044 使用合法 0.00005 s hop，没有复制帧、延长可用前缀或补造观测。全部通过 Sample、恢复比较、隔离工程原子提交和重开。大用例运行时版本保存在 cases.json；后续UI改动不影响headless处理路径，统一入口再次检查一致性；仅豁免该路径不构造的Session.cpp/.h，检测/结果服务/DSP等其余源码仍须一致。

60 s / 20 MS/s 合成持续信号在 96 MiB 授权下完成有界处理，RSS 约 19 MiB、实际 scratch 峰值约 8.94 GiB，全部 lease 归还。该结果明确走 measured-coarse fallback，Sample publicationEligible=false；它证明长信号执行与回退，不证明 Sample 活动段质量或现场 RF 精度。首次 300 s 超时及 900 s 重跑记录均保留。

统一入口 final-acceptance-06 返回0，R-01～R-20全部PASS。最终代码 Debug完整CTest一次60/60（988.28s），Release一次60/60（292.53s）。启动套件10个Qt行，包括接受后上下文切换、替换工程、序列化写入守卫，以及模态重命名期间工程替换。初始失败/超时/中断和旧覆盖记录均保留，不与本轮混称。

原生各20次准备反馈，Paint完成p95：Debug6.4826ms、Release37.6025ms。两配置的system-only PATH独立部署通过QRhi GPU与软件回退。FFSCN/SCN真前向及独立PyTorch golden、合法profile、注入B2→B1同窗两次前向、B1失败、分配前后取消均通过；注入不称真实物理OOM。

H1在daa30e8发现P0接受后回滚和P1通道编辑被覆盖，结论FAIL，未推送。独立C3和主代理修复了通道CRUD/导航/视图守卫、调用方拒绝处理及过期回调；已接受启动不再恢复旧工程，保留当前上下文且不覆盖新工程。重命名在模态返回后重新定位通道。原始FAIL与复现保留，见REVIEW_FIX_CLOSURE.md。

最终重新串行同条件20窗口对照：冷应用点击→首推理请求5.659→3.127s，正式发布12.976→10.627s（降低18.11%）；驻留重检首推理5.999→3.363s，正式发布10.290→7.948s（降低22.76%）。每侧3冷测、排除一次驻留预热后3重检，14次运行含预热均成功。8份重开工程中的9个正式对象几何与质量字段精确相等。详细阶段/资源范围见performance/CONTINUATION_PERFORMANCE.md与CONTINUATION_COMPARISON.json。

复现入口：`scripts/verify-ss-de-rpo.ps1`；原始请求生成 `scripts/prepare-ss-de-rpo-requests.ps1`；独立原始流程 `scripts/verify-ss-de-rpo-original.ps1`；性能 `scripts/benchmark-ss-de-rpo.ps1 -Native -Mode Both`；汇总 `scripts/summarize-ss-de-rpo.py`。基线补丁及说明见 `performance/BASELINE_REPRODUCTION.md`。

性能边界：每侧三个新应用进程冷测、同一应用排除一次预热后的三次驻留重检；每次独立 Worker/engine/context 初始化。OS 缓存未清空。计时从真实点击到观察到正式 slot，随后重开是未计时的验证门槛。采样 CPU/RAM 限本次父子进程，VRAM 为整块 NVIDIA 设备（包括其它用户），卷空间包括其它写入。各 host 阶段可重叠，不能相加当总耗时。原生反馈为 Paint dispatch 完成时间，DWM/扫描输出不在测量边界。

全部原始日志与中间产物位于 `out/tasks/ss-de-rpo/`。独立终审：`out/tasks/ss-de-rpo/H_FINAL_REVIEW.md` 与 `agents/results/H2/session.json`；发布记录：`out/tasks/ss-de-rpo/PUBLISH_RECORD.json`。这些文件在最终提交后生成，不再次提交含自身SHA的报告。原有 agent-roster.md 修改、历史未跟踪审查及压缩包均保留。

可复现完整验收（输出目录必须是新的空目录，原始IQ/模型/Golden需已提供）：

```powershell
./scripts/verify-ss-de-rpo.ps1 -Configuration Both -FullCTest -Native -AllowGpu -OutputDirectory out/tasks/ss-de-rpo/repro-final -ModelRoot out/vs2026-qt611-release_bin/detection-models -LiveGoldenRoot out/tasks/wideband-detection/M3/assets/epoch1200 -AdaptedGoldenRoot out/tasks/wideband-detection/R2/assets/dynamic-ffscn
```

最终依据：本文件、test-matrix.md、change-index.md、PRODUCTION_INTEGRATION.md 和新性能报告。旧 FINAL_DELIVERY.md / INDEPENDENT_REVIEW.md 是保留的历史资料，不能替代本轮精确HEAD的外部H记录。

本轮最终外部门禁是新的H2精确HEAD审查，尚未生成时不表示已获批准。H1原报告为out/tasks/ss-de-rpo/H_REVIEW_daa30e8_FAIL.md；最新H2及发布记录在最终提交后写入外部路径，避免自引用SHA漂移。
