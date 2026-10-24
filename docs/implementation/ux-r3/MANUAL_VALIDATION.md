# UX-R3 使用与复核入口

从 `out/vs2026-qt611-release_bin/SignalStudio.exe` 启动。文件 → 新建工程，在已存在的任意目录创建 `<名称>.ssproj`；添加/打开信号后进入宽带图谱。保存、另存为、最近工程、关闭工程位于文件菜单。另存为明确选择复制或引用外部伴随资产；不要单独移动 `.ssproj` 而遗漏其 `.ssproj.assets`（引用模式则须保留所引用资产目录）。

旧 v6：文件 → 导入旧工程 v6，预览文件/标记/通道/检测次数后另选新的 `.ssproj` 目标。原 JSON 及其资产不原地改写。v1–v5 仍不兼容。工程树根节点右键查看工程/资产路径或在资源管理器中显示；文件节点右键重新定位源文件。重定位核对原长度、格式、样本数及首尾内容签名，失败保持当前绑定。

显示侧板提供 Y 轴标题方案 A/B/C；默认 A 为单位顶置，主图和辅助图使用共享预算。波形/PSD、时频/瀑布可从各自标题栏切换。沿用轴滚轮、轴拖动、ROI 确认、标记选择/显式定位、图形编辑与游标。

导入信号后自动发现可信标签。DATA/TXT 只做 `data/name.data` ↔ `label/name.txt` 严格配对，RAW 的编码和 Fs/Fc 仍从普通导入预检确认。未知 TXT 逐项确认列、索引、区间和单位后预览导入；未知频率不造框，可确认只保存原始来源和诊断。文件菜单可重新发现当前信号标签，也可从 DATA/TXT 标签反查对应 DATA 后进入普通导入配置。底部“标签预览…”支持手动选择其它标签文件。来源变化时明确保留、比对或载入新版本，已有修订保留。没有标签仍可绘制手工标记或使用完整范围表单。

从检测结果或标记进入创建窄带通道配置，保留来源快照和默认 2B/倍率/手工 Fs 规则。窄带导航依次为观察、星座、眼图、调制识别、解调。顶部“同步与参数”按需展开共享设置；星座和眼图消费同一真实分析结果，各自调整显示。识别模型未部署时真实入口不可用；明确标示的合成演示不能作为真实识别/位流结果。

活动检测任务继续向提交时冻结的资产目录写入；复制资产的“另存为”须等待任务结束或取消完成。明确选择引用现有资产目录的另存仍保留这条写入路径，不会静默切换 worker 的目录。

Windows `.ssproj` 文件关联为独立部署项，可先运行 `scripts/ssproj-association.ps1 -WhatIf` 查看将注册的当前用户配置；部署后运行该脚本注册，`-Unregister` 撤销本程序注册。已有默认程序不被强行替换，可在“打开方式”选择 Signal Studio。命令使用带引号的 `SignalStudio.exe --project "%1"`，支持带空格路径。

部署六组合复核使用 `scripts/verify-ux-r3-deployment.ps1 -Configuration Release -AnalysisProject <已保存的真实恢复眼图.ssproj> -OutputDirectory <新目录>`。它分别验证宽带、内置窄带五页、真实眼图工程的 GPU/软件启动。内置五页验收显式提供 1 MHz 手工 Rs 假设并写入报告，不把该值当作样例的真实符号率；普通启动仍须用户确认 Rs。文件关联实际控制测试为 `scripts/test-ssproj-association.ps1 -Project <工程.ssproj> -ReportPath <报告.json>`，仅在本程序 ProgID 不存在时运行，完成后注销本程序注册。

构建/全量测试：`scripts/verify-ux-r3.ps1 -Action BuildTest -Configuration Debug`，Release 同理。原生轴矩阵：`-Action Native`，`-SoftwareRenderer` 另跑软件。窄带原生：`scripts/screenshot.ps1 -NarrowbandAnalysis -Configuration Release -OutputDirectory <新目录>`；包验证：`scripts/verify-ux-r3.ps1 -Action Package -Configuration Release`。实际结果、失败记录与硬件身份以 VALIDATION 为准。
