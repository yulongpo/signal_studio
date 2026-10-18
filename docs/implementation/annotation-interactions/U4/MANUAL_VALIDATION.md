# 标记交互与窄带通道手动验证

本文件提供少量代表性样本的复核步骤及现有脚本入口，不表示这些
步骤已由用户执行。U4 已完成 Debug/Release 各 23/23 CTest、108/108
Qt UI，0 失败/跳过；正式模型原生 Debug 43/43、Release 46/46，
Release U1/U2/U3 8/8、10/10、12/12，8 种 GPU/软件部署模式和两项
不可用情景均通过。自动运行的证据与能力边界见[交付说明](DELIVERY.md)、
[验证记录](VALIDATION.md)及[原生验收](NATIVE_ACCEPTANCE.md)。新运行使用新目录，保留实际
程序、样本、模型、屏幕身份、原始错误和结果；不覆盖旧 U1–U3、
R1–R6 证据。

## 准备与启动

使用已经构建部署的 `out/vs2026-qt611-release_bin/SignalStudio.exe`。
Debug 对应 `out/vs2026-qt611-debug_bin/SignalStudio.exe`。普通标记与
CPU DDC 不要求模型就绪；执行正式检测前应按
[模型部署](../../wideband-detection/R6/MODEL_DEPLOYMENT.md)准备匹配包。

新建一个独立测试工程，再添加一份可读的复数 IQ。当前代表样本为
用户本机 `resources/sigmf_data_demo/0000000.sigmf-data` 及同名 meta；
clone 不保证附带该文件。核对 `cf32_le`、400000 样本、20 MS/s、
20 ms、Fc 2.5 GHz、4 条原始标签，IQ SHA-256 为
`3a47e55b5f02851d702d5c5782998bf3e374f67cde592c35689acf9ec5399254`。
meta 的描述是 TorchSig 合成基准，不能记录成现场 RF 实采。

文件只读导入，工程和运行产物另存。检查 Fs、Fc、有效带宽、物理长度
与实际标签坐标，RF 坐标不重复加 Fc。导入标签目前仅覆盖指定
TorchSig SigMF annotations 和本任务生成器 TXT；不把其它标签格式
视为通用支持。生成器 WAV 须保持 Fs/格式与容器一致；旧工程若有
`检测源 WAV 的 Fs 或格式与容器冲突`，重新导入修复元数据，不改写
身份或关闭校验。

原生显示复核使用用户当前指定的 U27U2D / DISPLAY2、连接序号 1，
3840×2160 物理、150% DPI、2560×1440 逻辑全屏。连接顺序可能变化，
以本次 `--list-screens` 探针和实际屏幕为准，不沿用旧 Redmi / DISPLAY6。
有多个 4K/150% 候选时须明确选择实际外接屏。

## 少量操作复核

先在实际文件上检查原始标签和一个手动标记。实际模型可能正常返回
零结果；不能为凑齐三来源而把手工对象命名成模型输出。缺少多段、
未知时间或大列表条件时，将该项记为未覆盖，并用下一节现有原生
夹具补充交互覆盖，夹具与实际模型运行分别记录。

| 操作 | 期望观察 |
|---|---|
| 图、表、左树依次选择同一对象；Ctrl/Shift 选择，切当前段 | 三处所选 ID 与当前段一致；对象多段全部高亮，仅当前段有控制点；单击不缩放，定位才改变视区；悬停不触发谱计算 |
| 排序、翻页、筛选、关层，再使用“全选匹配”或 Ctrl+A | 全选覆盖当前筛选全部页；筛选/关层清除不匹配的选择；按需加载或定位可找到左树第 100 条以外对象；不删除隐藏对象 |
| 分别对原始标签、手动和算法对象双击/F2 编辑 | 三来源同等可编辑；提交保留 annotationId 和 sourceKind，产生新修订；原文、原始几何和模型证据不变 |
| 输入草稿，穿插刷新、输入框 Delete、Esc、切文件及窗口失焦 | 输入和刷新不提交；Delete 只删文字；Enter/Tab 或应用内切焦点提交；Esc、切文件取消，窗口失焦不应用草稿 |
| 打开完整范围编辑，先改多个字段再应用；拖动当前段控制点 | 未应用前业务范围和修订不变；整组校验后一次提交，避免非法中间态；只改当前段，一次撤销恢复 |
| 修改时间未定位对象的频率 | 时间仍显示未知、无伪造活动段；观察支持不变；只显示频率编辑能力 |
| 多选并删除，先取消再确认 | 预览对象/段/通道数量准确，取消不提交草稿；确认一次软删除、一次撤销；保留删除后到达的后台结果；最后一段删除提示对象删除 |
| 将质量设为待判断或已忽略，再编辑和创建通道 | 质量不阻止入口；名称/备注编辑保留质量，几何变化恢复待判断；不会为采纳复制对象 |
| 对多段对象从表、树或图打开创建通道 | 一次绑定明确的当前段；多选要求表单明确指定一个来源；表单显示 LocatedSegment、完整绑定范围和实际裁剪范围 |
| 对未知时间对象创建通道；将视区移到可用前缀外 | UserRange 默认视区与前缀交集；空交集仍开表单并说明具体原因；只有按“显式使用可用前缀”才扩大范围；创建不写回算法时间或 segmentId |
| 配置合理的中心/带宽/输出率/滤波并打开窄带 | 预览真实 CPU DDC/FIR/L/M 计划，字段失败有具体原因；实际波形、PSD/STFT 可读；不把合成星座/眼图/识别/解调当真实结果 |
| 已建通道后改名称、备注、质量、兄弟段，再改绑定段、删除和撤销 | 前四项保持 Current；绑定几何/物理来源变化为 Changed，绑定对象/段缺失为 Missing，恢复相同绑定后 Current；创建快照不变、不自动重算 |
| 保存、关闭并重开 v6 工程 | 已提交范围、质量、修订、未知时间、UserRange、通道创建快照与来源关系保留；原始证据不变，实际 IQ 输出可按相同范围复核 |

Loading/Partial 单独记录状态、物理样本数及 `availableSamples`。
非空前缀内的通道可以执行，提取结束不能超过前缀；空前缀、Failed、
实数源、非法保护带/输出率、物理来源改变均应保留具体原因。
真实停止载入需要可重复的大文件和 SourceLoader 停止过程；完整文件上
手动控制前缀只证明受控边界，不能冒充实际停止载入。

## 旧工程升级复核

使用一份独立测试副本打开真实 v1–v5 工程，保持原始 IQ 路径及其附属
分片可读。保存前记录原 `project.json` 的字节数与 SHA-256，确认旧通道
中心/带宽/时段/输出率和快照；不要用当前标记重建旧通道。

首次保存后核对 v6，及同目录
`project.json.pre-v6-v<原版本>-<原始字节SHA256>.bak`。备份须与保存前
字节逐一一致，再次保存不增生相同备份；发生同名不同内容时不得覆盖。
保存失败应保留原工程，不出现部分替换。重开后核对旧参数、ID、来源
证据和通道快照。当前冻结 v5 夹具引用旧 W: 路径，只能在相同来源
可读时执行旧通道；不能改路径身份后宣称验证了原快照。

## 现有自动复验入口

以下从仓库根目录执行。`manual-v1` 必须未使用，再次复验换成
`manual-v2` 等新目录。脚本顺序执行，图形验收期间使用实际屏幕和
原生 DPI；不设置 `QT_SCALE_FACTOR` 模拟目标屏幕。

```powershell
$ErrorActionPreference = 'Stop'
$manualRoot = [IO.Path]::GetFullPath('out/tasks/wideband-detection/U4/diagnostics/manual-v1')
if (Test-Path -LiteralPath $manualRoot) { throw 'Use a new manual validation directory' }
New-Item -ItemType Directory -Path "$manualRoot/logs" -Force | Out-Null
```

只有改动源码、工具链或需要重新核对部署身份时才重跑构建。
`build.ps1 -Action All` 使用既有配置、构建及 CTest runner，UI 保护仍为
480 秒。CMake/Qt/VS/TensorRT 配置见 [README](../../../../README.md)。

```powershell
./scripts/build.ps1 -Configuration Debug -Action All *> "$manualRoot/logs/build-debug.txt"
./scripts/build.ps1 -Configuration Release -Action All *> "$manualRoot/logs/build-release.txt"
```

标记交互复用 U1、U2、U3 原生模式；没有 U4 参数。本轮在 Release
完成这些模式，以下提供相同范围的复验入口；Debug 没有本轮 U1–U3 结果。它们使用固定小型
实际 IQ 和明确的 UI 标记夹具，分别覆盖 8、10、12 个场景；U3 执行
3 次真实 CPU DDC、v6 重开及旧版备份，不执行检测模型。固定输入或
迁移分片缺失时，保留缺失原因，不能修改冻结 hash 或绕过存储校验。

```powershell
$configRoot = "$manualRoot/release"
foreach ($node in @('U1', 'U2', 'U3')) {
    ./scripts/screenshot.ps1 -Configuration Release -AnnotationInteractions $node `
        -OutputDirectory "$configRoot/native-$($node.ToLowerInvariant())" `
        *> "$manualRoot/logs/Release-native-$node.txt"
}
```

以下复验 Debug/Release × 宽/窄带 × GPU/软件共 8 种模式的包验证和
smoke。本轮两类入口各 8/8 通过，原报告见[显示与部署归档](renderer/)。保持上方
`$manualRoot` 为绝对路径，避免包验证切换工作目录后改变报告位置。脚本从对应
`_bin` 运行并移除 Qt 安装路径，使用 system-only PATH；演示数据仅用于
显示与部署验证。GPU 报告应有非 CPU QRhi 驱动、实际纹理和顶点上传、
draw call 及完成帧；软件报告应明确不是硬件 renderer。它们均不证明
TensorRT 已运行或提供 CPU 模型推理。

```powershell
foreach ($configuration in @('Debug', 'Release')) {
    $configRoot = "$manualRoot/$($configuration.ToLowerInvariant())"
    ./scripts/verify-package.ps1 -Configuration $configuration `
        -ReportPath "$configRoot/package-gpu.json" *> "$manualRoot/logs/$configuration-package-gpu.txt"
    ./scripts/verify-package.ps1 -Configuration $configuration -SoftwareRenderer `
        -ReportPath "$configRoot/package-software.json" *> "$manualRoot/logs/$configuration-package-software.txt"
    ./scripts/verify-package.ps1 -Configuration $configuration -Narrowband `
        -ReportPath "$configRoot/package-narrowband-gpu.json" *> "$manualRoot/logs/$configuration-package-narrowband-gpu.txt"
    ./scripts/verify-package.ps1 -Configuration $configuration -Narrowband -SoftwareRenderer `
        -ReportPath "$configRoot/package-narrowband-software.json" *> "$manualRoot/logs/$configuration-package-narrowband-software.txt"
    ./scripts/gpu_smoke.ps1 -Configuration $configuration `
        -OutputDirectory "$configRoot/gpu-wideband" *> "$manualRoot/logs/$configuration-gpu-wideband.txt"
    ./scripts/gpu_smoke.ps1 -Configuration $configuration -Narrowband `
        -OutputDirectory "$configRoot/gpu-narrowband" *> "$manualRoot/logs/$configuration-gpu-narrowband.txt"
    ./scripts/gpu_smoke.ps1 -Configuration $configuration -SoftwareRenderer `
        -OutputDirectory "$configRoot/software-wideband" *> "$manualRoot/logs/$configuration-software-wideband.txt"
    ./scripts/gpu_smoke.ps1 -Configuration $configuration -Narrowband -SoftwareRenderer `
        -OutputDirectory "$configRoot/software-narrowband" *> "$manualRoot/logs/$configuration-software-narrowband.txt"
}
```

需要单独复核正式模型全流程时，先确认兼容的 worker、模型及 NVIDIA
驱动，再在新目录运行既有入口。本轮已完成 Debug 43/43 和 Release
最终验收 46/46，以下可复验相同范围；完整失败和零结果照实记录。

```powershell
./scripts/screenshot.ps1 -Configuration Debug -WidebandDetection -ProductionModel `
    -OutputDirectory "$manualRoot/model-native-debug" *> "$manualRoot/logs/model-native-debug.txt"
./scripts/screenshot.ps1 -Configuration Release -WidebandDetection -ProductionModel -FinalAcceptance `
    -OutputDirectory "$manualRoot/model-native-release" *> "$manualRoot/logs/model-native-release.txt"
```

本轮实际前向为 FFSCN，每配置的 Sample 与 ROI 均各 3 个窗口；未新增 SCN
前向。此入口不能替代 G/T 全矩阵、SCN 或各统计谱的独立精度评价。
模型当前验证包为 TensorRT 10.11 / RTX 5060 / SM 12.0，
换 GPU 不能默认复用 engine。批量 G/T 仍由用户按
[R6 手动验证](../../wideband-detection/R6/MANUAL_VALIDATION.md)建立新
freeze、新结果目录后决定运行；本轮不启动大量效果矩阵。

## 结果记录

每项填写通过、失败或未覆盖，关联实际日志、JSON、截图、工程及其分片。
记录程序/样本 SHA-256、源格式与可用前缀、通道绑定方式和提取范围、
DDC 输出数量/能量/哈希、模型标识与实际推理设备、屏幕身份和 renderer。
模型前向、显示 GPU、CPU DSP、合成 UI 夹具分别记录。

解释性能时分别记录可见反馈和取消完成：本轮 Debug 正式模型启动
566.3617 ms，未达 200 ms 建议目标；Release 为 134.9452 ms，最终
真实任务启动/取消可见反馈为 113.0498/157.9721 ms，取消完成
179.2669 ms。十万合成段选择/筛选 P95 为 84.2224/70.4745 ms，
各 20 次，选择含图表帧、筛选不含；Debug 没有最终取消/压力结果。
不能将这些少量测量转述为所有配置、连续采集或真实 RF 性能通过。

遇到问题保留首次失败和原始错误，不覆盖成后来成功。功能或代表性
前向通过不表示真实 RF 精度、全部 L3/L4、跨设备吞吐、连续采集或
冷/热启动性能达标；未覆盖项继续标明未覆盖。
