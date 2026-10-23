# 2026-10-23 星座同步与数值上调反馈修复

本轮针对用户两张截图实际修改 C++/Qt 和 DSP。基线是已交付 W8 的本地 62739d9；反馈验收完成时尚未提交或推送，随后用户明确授权同步，按功能分支 → rebuild/a143-native → main 交付，见[同步说明](SYNC.md)。原 W8 证据保留。当前 CTest 新增两个独立测试，库存 37 项；本轮执行相关定向回归，没有声称重新执行全部 37 项。

## 已查实原因与改动

实际 WAV 的生成器 TXT 标签在约 9.795561 MHz 的目标上写明 **32QAM，FD=4891160，BW=6603066**。用户提取通道中心约 9.962746 MHz，故粗频偏约 -167185 Hz。程序估计 Rs≈4891170.562 Sym/s 与标签吻合，失败原因是原先只有 BPSK/QPSK/16QAM 模板，选择 QPSK 仍不匹配这段 32QAM。

原界面选择“载波恢复”但失败后继续显示 raw，标题与下拉框没有同时解释实际阶段，容易误认为操作未执行。现在按钮明确命名“同步恢复”，采用主操作样式；未知格式时提示先选调制假设。失败时显示已产生的最远阶段，图内明确标出请求/实际阶段，保持 raw 可单独选择，不以失败回退冒充同步成功。

Windows 11 原生样式的步进按钮横向排列；应用 QSS 使内嵌 QLineEdit 覆盖上调按钮。首次原生测试确实得到“点击上调后仍为 0.35，预期 0.45”，同时 `childAt(up.center)` 为 QLineEdit。offscreen 样式没有覆盖，原先以 setValue/键盘为主的验收漏掉了此路径。现所有 ParameterSpinBox 为步进按钮保留空间，去除内嵌编辑器多余边框/留白。数值上下调直接提交；键盘草稿、禁滚轮、窗口失焦不提交及上下文切换撤销规则保留。环路带宽的单步值改为 0.0001。

用户补充的眼图遮挡也已定位：固定内联控件与后接入的 ChartControlStrip 同时占用标题栏；原有 92/100 像素数值框在为原生双按钮预留空间后进一步挤压文字。两图现在完整复用 PSD/STFT 的 ChartControlStrip 和 SettingsPopoverHost，标题高度均为 30 逻辑像素、常用项右对齐、同一套自动溢出。星座高级参数（去 DC、归一化、手动轴范围）和眼图高级参数（密度、折叠偏移、RRC 滚降）进入各自“更多设置”；数值框完整显示数值和单位。设置层限定于本图绘图区，编辑器始终为同一实例，刷新、Esc、外部点击和上下文切换沿用公共草稿规则。

## 支持范围与参数条件

| 调制族 | 阶数 | 判决几何 |
|---|---|---|
| M-PSK | 2、4、8、16、32、64、128、256 | 等角相位，保留固有相位模糊 |
| M-QAM | 4 至 4096 的 2 次幂 | 8 矩形；32/128/512/2048 交叉形；其余方形 |
| M-APSK | 8、16、32、64、128、256 | 可编辑 `点数:相对半径:相位角度` 环配置 |

APSK 16/32/64 预设环数分别为 4+12、4+12+16、4+12+20+28；半径/角度是分析假设，可编辑。8/128/256 采用通用同心环预设，不宣称某个 DVB MODCOD 的完整一致性。几何依据：[MathWorks QAM 星座形状](https://www.mathworks.com/help/comm/ref/qammod.html)、[ETSI APSK 环结构](https://www.etsi.org/deliver/etsi_en/302300_302399/30230702/01.01.01_60/en_30230702v010101p.pdf)。实现独立，未复制外部业务代码。

新增判决器执行盲相位搜索和判决导向环；PSK/QAM 的快速判决与穷举最近距离逐例比对。APSK 自动 CFO 用匹配滤波、初始定时与各环循环矩共同消除单环频率歧义，不只看散点外观。旧 BPSK/QPSK/16QAM 路径和固定数值门槛保留。

50 个独立发射正例覆盖 25 种阶数 × 理想/加噪，该矩阵手工 Rs=625 kSym/s；自动 Rs 另由既有 24 个数值正/负例、新增三种调制族 Qt 流程和用户 WAV 复现验证。42 个自动粗 CFO 与 8 个手工粗 CFO 条件分别记录于数值 JSON；高阶 PSK（M≥32）用手工粗 CFO，M=32 噪声为源 SNR30dB，M≥64 为40dB；QAM M≥512 为源 SNR40dB、RRC跨度20、定时环带宽0.0005，其余加噪正例为源 SNR20dB。界面不偷偷重写这些用户参数，不能把支持某阶数解释为任意 SNR/频偏下必然锁定。

新阶数真符号 EVM 门槛为 `min(18%, 0.45 × 归一化最小星座距离)`，CFO误差≤0.001Rs、定时RMS≤0.10UI、有效符号>4096。仅统一正增益、延迟和固有相位模糊对齐，未逐点旋转/重排。六种纯噪声假设不报载波锁定；新增字段严格往返/拒绝，旧 v6 枚举值 0–6 保持原义。

提交前 DSP 复审另外发现极小 APSK 相对半径的平方下溢问题。先用 `1e-200` 半径实际复现非有限点，再补低功率预缩放及成功返回前的有限值检查；新增 `1e-200` / `1e-300` 几何、单位功率和 v6 往返用例。修正后两配置相关 6/6 回归及原 50 个调制正例再次通过，详见 [REVIEW.md](REVIEW.md)，没有更改有效常规配置的归一化路径或数值门槛。

## 验证与复验

最终结果已归档到 [SUMMARY.json](evidence/SUMMARY.json)，两配置分别记录，不以历史 W8 日志代替本轮结果。

| 检查 | Debug | Release |
|---|---|---|
| 相关 CTest（输入、调制、恢复、Rs/眼图、采样率、Qt） | 6/6 | 6/6 |
| 新增调制族独立真值正例／纯噪声假设 | 50/50；6/6 不误锁 | 50/50；6/6 不误锁 |
| Qt 窄带流程（9 个业务行，含初始化/清理） | 11/11 | 11/11 |
| 共享谱图、布局、设置层与两项参数方法定向回归 | 4/4 CTest | 4/4 CTest |
| Windows 原生上下调及草稿单元测试 | 5/5 | 5/5 |
| Redmi 实屏 GPU／软件原生流程 | 各 7/7 | 各 7/7 |
| 输出目录、仅系统 PATH 的独立启动 | GPU／软件均通过 | GPU／软件均通过 |

各配置 50 个独立正例的最大真值 RMS EVM 为 **3.0745%**，最大定时 RMS 为 **0.017970 UI**，最大残余 CFO/Rs 为 **9.345×10⁻⁶**；逐例门槛见 [Debug 数值](evidence/debug-modulation.json)、[Release 数值](evidence/release-modulation.json)。既有 13 个恢复正例、8 个不适用/负例和 24 个 Rs 数值用例也在上述相关 CTest 中通过。完整 37 项 CTest 没有在本轮重跑，原 W8 的完整回归证据保留。

本轮四组原生流程共 **28 个场景**。本次探针均为 **Redmi 27 NU，连接 2，`\\.\DISPLAY6`，物理/首选 3840×2160，Qt 2560×1440，DPR=1.5**；未使用替代屏幕或伪造超大窗口。每个场景检查星座、眼图、STFT 标题栏高度、右对齐、几何不重叠及原生命中，展开场景另外检查数值和单位的完整可见性。GPU 路径实际为 QRhi D3D11，并记录纹理、顶点上传和数据绘制；软件路径单列。截图：[眼图更多设置](assets/release-gpu-04-eye-settings.png)、[星座更多设置](assets/release-gpu-05-constellation-settings.png)、[恢复后的 32QAM](assets/release-gpu-32qam.png)。

用户 WAV 的 QPSK 负例与 32QAM 正例均实际读取同一源数据；Rs≈4.89117 MSym/s，32QAM 定时与载波锁定，条件 EVM 约 **8.7%**（探针预处理配置 8.74%，Qt 场景 8.72%，各自报告保存原值）。标签仅用于验收对照，不进入接收算法。该数据来自生成器，不扩大为 RF 实采精度。原生场景用 API 建立独立工程/通道夹具，QPA 驱动假设、同步、设置层、真实上调和保存重开，未修改用户工程或原始 IQ。

失败记录保留于 `evidence/failed-*`：首次原生上调失败的原始固定日志曾被早期复跑覆盖，现有 [现场摘录](evidence/initial-input-failure-excerpt.txt) 明确标明转录来源；不将其冒充完整原始日志。编译失败、调制算法迭代失败与后续通过分开保存。布局首轮测试错误地要求通过页签 API 切换时丢弃草稿，改为验证产品规定的真实通道上下文切换；未改变输入规则。另一次 Debug 命令误写两项 Qt 方法名，完整失败输出保留，使用真实方法名后补跑 4/4 通过。没有放宽原 W0 数值门槛。

```powershell
./scripts/verify-narrowband-feedback.ps1 -Configuration Debug -RebuildGui
./scripts/verify-narrowband-feedback.ps1 -Configuration Release -RebuildGui
./scripts/verify-narrowband-feedback.ps1 -Configuration Release -NativeInput
# 本轮专用 WAV 复现，UserSource 填截图对应 WAV 绝对路径（不是通用任意 WAV 探针）
./scripts/verify-narrowband-feedback.ps1 -Configuration Release -NativeScene -UserSource '<WAV绝对路径>'
./scripts/verify-narrowband-feedback.ps1 -Configuration Release -NativeScene -SoftwareRenderer -UserSource '<WAV绝对路径>'
./scripts/build.ps1 -Configuration Debug -Action All -Targets SignalStudioUiTests,SignalStudioUxTests -TestRegex '^signal_studio_(ui_parameters|ux_(popovers|spectral|layout))$' -UiCases parameterDraftsRequireExplicitCommit,parameterInputsIgnoreWheel
./scripts/archive-narrowband-feedback.ps1
```

用户操作：重新启动更新后的 Release，选择 **M-QAM → M=32 → 同步恢复**。当前源 Rs 自动估计可用；预期 Fs≈13.54 MS/s、Rs≈4.89117 MSym/s、频偏≈-167.23 kHz、条件 EVM≈8.74%。条件 EVM 与独立发射符号真值 EVM分别记录。
