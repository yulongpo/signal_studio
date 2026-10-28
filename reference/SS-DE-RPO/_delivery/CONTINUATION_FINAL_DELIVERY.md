# SS-DE-RPO 续作开发与验收交付

开发基线7220b2e，最终实现代码4898fa4，分支rebuild/cursor。资源治理、Sample、准备取消、确定性频率索引、正式发布恢复、准备复用和真实GPU路径均已接入。H1/H2审查发现的阻断及后续QA问题均已修复并补验。最终审查SHA、结论与推送事实保存在提交外；独立H3必须在本证据提交后对精确HEAD给出PASS，才能正常推送。

可运行程序：out/vs2026-qt611-release_bin/SignalStudio.exe；Debug对应out/vs2026-qt611-debug_bin/SignalStudio.exe。Qt6.11.1 / VS2026 x64 / C++20。Git不包含部署模型、CUDA/TensorRT/Qt DLL和用户原始IQ；本机部署已实际独立启动。

环境：Ryzen AI9 H365、20逻辑处理器、约64GiB RAM；TensorRT10.11.0.33 / CUDA12.9 / RTX5060Laptop SM12、8GiB VRAM。QRhi D3D11显示为AMD Radeon880M，与NVIDIA推理分开。当前原生探针唯一Redmi27NU、连接2、DISPLAY6、3840×2160、150%，逻辑全屏2560×1440。

统一入口final-acceptance-07退出0，22个门禁（R-01～R-20及两种完整CTest）全部PASS。Debug完整CTest一次60/60，1052.54秒；Release一次60/60，294.04秒。没有复用旧GUI覆盖。启动套件16个Qt总行（14个业务/数据行及初始化/收尾），包含双文件读取与两种提交、接受后的上下文/工程切换、关闭时GUI确认等待、保存互斥、来源/筛选/选择冻结、模态重命名。完整工作流增加标签预览/重载/比对及另存为三个竞争边界。

原始IQ来自用户现有0.8秒、20MS/s交替I16 WAV（实际来源见N1请求和性能JSON，中心频率按请求为0，不从文件名推断650MHz）。四种SourcePlan窗口数均在4898fa4重新运行：

| 窗口 | 完整执行时间 | 结果 |
|---|---|---|
|20|9.705秒|推理、Sample、正式提交/恢复比较/重开PASS|
|159|43.410秒|同上，159/159|
|363|108.892秒|同上，363/363|
|2044|531.125秒|同上，2044/2044|

2044使用合法0.00005秒hop，由SourcePlan真实产生，不复制帧补数。2044父进程实测peakRSS745312256字节，约710.79MiB（不含独立Worker）；其余资源见原始measurement。上述是功能执行时间，不是原生点击性能对照。20窗口raw-head ON实存3932160字节；OFF及预算不足拒绝契约均通过。

60秒/20MS/s合成持续信号的既有专用DSP helper测量在96MiB授权下完成，RSS约19MiB、scratch峰值9600000606字节，全部lease归还。该helper未受本轮UI/选择服务修复影响，原执行版本及原始结果保留在N3/sample-actual-fs-60-retry；没有声称在4898fa4重新做过这一测量。它明确是measured-coarse fallback、publicationEligible=false，仅证明有界执行及回退，不证明Sample活动段质量或现场RF精度。新的原始IQ大窗口及本轮SampleGolden是独立证据。

真实FFSCN、SCN前向和独立PyTorch Golden通过；只使用合法profile。FFSCN物理batch2，SCN物理batch1，逻辑batch2为两次独立enqueue。受控B2→B1同窗重试确实完成两次前向，B1失败及分配前后取消/释放检查通过；注入故障不称真实物理OOM。

原生各20次准备反馈的Paint dispatch完成p95：Debug6.95432ms、Release54.4546ms，均低于100ms。handler/queued callback另记，不包含DWM合成/扫描输出。Debug/Release均通过system-only PATH的宽带GPU/软件独立启动，以及窄带演示5页的GPU/软件独立启动。演示原生与CPU DSP证据不替代原始RF或模型精度。

最终串行20窗口原生对照：冷应用点击→首推理请求5.616→3.067秒，正式slot12.930→10.555秒，耗时降低18.37%；驻留重检首推理5.873→3.293秒，正式slot10.193→7.757秒，降低23.90%。每侧3冷测、排除1次驻留预热后3重检；14次含预热均成功。8份重开工程中9个正式对象的几何和质量字段精确相同。每次仍独立Worker/engine/context初始化，OS缓存未清空。它不外推到2044窗口或现场吞吐；完整阶段/资源/采样边界见performance/CONTINUATION_PERFORMANCE.md和CONTINUATION_COMPARISON.json。

H1在daa30e8发现接受后回滚P0、通道编辑丢失P1；H2在83e365f发现其它文件读取完成被提交覆盖P1，均FAIL且未推送。C3/C4/C5/C6和主代理完成通道及来源状态守卫、标签模态重定位、窄带/选择写入治理。主代理另修复保存与自动发布竞争、关闭时相互等待和发布快照循环引用。红/绿复现、原超时及全部历史中断均保留，见REVIEW_FIX_CLOSURE.md及agents/C各交付。源格式、物理坐标、模型数学语义和1GiB连续分配保护保持兼容。

复现完整验收（输出目录须为新的空目录，实际IQ/模型/Golden须已提供）：

```powershell
./scripts/verify-ss-de-rpo.ps1 -Configuration Both -FullCTest -Native -AllowGpu -Jobs 8 -OutputDirectory out/tasks/ss-de-rpo/repro-final -OriginalRequestDirectory out/tasks/ss-de-rpo/N1/repro-requests -ModelRoot out/vs2026-qt611-release_bin/detection-models -LiveGoldenRoot out/tasks/wideband-detection/M3/assets/epoch1200 -AdaptedGoldenRoot out/tasks/wideband-detection/R2/assets/dynamic-ffscn -BenchmarkRequestJson out/tasks/ss-de-rpo/N1/repro-requests/iq-20-raw-off.json
```

其它入口：prepare-ss-de-rpo-requests.ps1生成请求；benchmark-ss-de-rpo.ps1 -Native -Mode Both测原生冷/驻留；summarize-ss-de-rpo.py汇总；verify-package.ps1检查独立部署。基线可复现补丁及说明见performance/BASELINE_REPRODUCTION.md。原始日志out/tasks/ss-de-rpo/N4/final-acceptance-07、performance-{baseline,current}-03和performance-summary-03.json。

最终独立审查out/tasks/ss-de-rpo/H_FINAL_REVIEW.md、agents/results/H3/session.json，发布记录out/tasks/ss-de-rpo/PUBLISH_RECORD.json，均在最终提交后写入，不再次提交含自身SHA的报告。必要时修复、补验并重新审查。当前文档不预先代表H3已PASS或远端已更新。原有agent-roster.md修改、历史未跟踪审查/压缩包均保留；旧FINAL_DELIVERY.md/INDEPENDENT_REVIEW.md不替代本轮最终门禁。
