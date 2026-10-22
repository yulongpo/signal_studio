# 复验入口

所有指令从仓库根目录执行。输出目录使用新名字，保留上次结果。需要 VS2026、Qt 6.11.1 EXACT 与已配置本地 preset；测试数据生成需要 Python/numpy/scipy，应用运行不需要 Python。

```powershell
./scripts/verify-narrowband-analysis.ps1 -Node W0 -GenerateFixtures -GenerateEdgeFixtures -GenerateRecoveryFixtures -GenerateRateFixtures -FixturesOnly
./scripts/verify-narrowband-analysis.ps1 -Node W7 -Configuration Debug
./scripts/verify-narrowband-analysis.ps1 -Node W8 -Configuration Debug
./scripts/verify-narrowband-analysis.ps1 -Node W8 -Configuration Release
./scripts/verify-narrowband-analysis.ps1 -Node W8 -Configuration Release -ProbeScreens
./scripts/verify-narrowband-analysis.ps1 -Node W8 -Configuration Release -Native
./scripts/verify-narrowband-analysis.ps1 -Node W8 -Configuration Release -Native -SoftwareRenderer
```

W8 全量包含 35 项 CTest。主 UI 三组并集是原 106 个业务 data 行，每组 480 秒终止保护保留；不要把超时/中断写成通过。定向复验支持 `scripts/build.ps1 -Targets ... -TestRegex ... -UiCases ...`；完整主 UI 筛选为 `^signal_studio_ui(_.*)?$`。W8 入口会清除继承的 SS_UI_CASES，归档核对冻结行集合，均从脚本进入。

原生捕获必须找到唯一 Redmi 27 NU：Windows 活动设备友好名和设备路径、首选物理模式与当前尺寸 3840×2160、Qt 2560×1440、DPR 1.5。默认 12 场景覆盖两种实际文件格式、倍率预览、DDC、无 Rs 原始星座、手工眼图、手工/自动 Rs 恢复、v6 重开和取消。捕获脚本从部署目录运行，清除 DPI/Qt 环境覆盖并使用系统 PATH。

没有指定实屏时返回阻塞。`-AuxiliaryDisplay` 只允许另行收集明确标注 `nativeAcceptance=false` 的渲染证据；不得用来完成指定实屏验收。本轮最终结果查看 VALIDATION.md。

从原生报告 `projects` 选择生成的 ci16/cf32 工程，可复验真正应用的独立启动：

```powershell
$project = '填写本次原生报告 projects 中的绝对 project.json 路径'
./scripts/verify-package.ps1 -Configuration Release -AnalysisProject $project -ReportPath 'out/tasks/wideband-detection/NB-W8/diagnostics/manual-package-gpu-v1.json'
./scripts/verify-package.ps1 -Configuration Release -AnalysisProject $project -SoftwareRenderer -ReportPath 'out/tasks/wideband-detection/NB-W8/diagnostics/manual-package-software-v1.json'
./scripts/run.ps1 -Configuration Release
```

手动检查：20 MS/s、1 MHz 默认 2 MS/s；依次选倍率和手工值，确认调整/拒绝原因；原始星座与眼图使用真实样本；未确认 Rs 为空，手工 Rs 不冒充锁定；625k Sym/s 时实际 SPS=3.2；切换五种阶段和 I/Q；关闭匹配/定时/载波检查联动；输入未提交时刷新和切换上下文；取消、保存重开、标记撤销/重做；噪声/短突发/跳频和不适用体制保留 raw 并不报可信锁定。

固定 seed 文件属于独立发射器生成的真实 IQ 数据文件。现场 RF 精度、未知制式自动识别、真实位流和 Ubuntu 不在通过范围内。

归档使用 `scripts/archive-narrowband-delivery.ps1`；若完整回归含已修复失败，必须用 DebugRetestLogs/ReleaseRetestLogs 显式提供对应通过日志。脚本逐个核对失败测试名、CTest 实际配置、完整 UI 行集合和最终实屏/独立启动证据，不接受定向 UI 子集或跨配置补测。`-VerifyCoverageGate` 可复验集合拒绝门槛；`-VerifyRetestGate -DebugRetestLogs <实际通过日志> -ReleaseRetestLogs <实际通过日志>` 可复验配置匹配和交叉拒绝门槛。
