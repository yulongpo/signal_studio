# 频谱参数、补零与加载验收（2026-10-10）

本轮在 `rebuild/a143-native` 实现独立 PSD/STFT 参数、完整区间谱统计、短数据补零、真实顺序读入及停止前缀、导航包络与中心保持、自定义下拉框、提示框修复，并统一所有勾选框颜色。截图复核另修正了窄带隐藏分析页显示时的热图目标矩形；调制页和识别页均验证了实际绘图区覆盖。

Debug/Release 构建及 CTest 均 **5/5 通过**，Qt UI 各 **80 passed / 0 failed / 0 skipped**。最终 CTest 耗时为 Debug 135.39 s、Release 39.54 s。原生加载/交互验收两配置均通过；8 组宽窄带、硬件/软件 smoke 均通过。结果汇总为 [summary.json](summary.json)，各日志、原始报告及截图保存在本目录。

## 验收对应关系

| 要求 | 实现及验证证据 | 结果 |
|---|---|---|
| FFT 从 32 点起步；频段变化重算 | 5–16 阶控件、可见频段 N 点、频段分析数值测试及频率缩放原生检查 | 通过 |
| PSD/STFT 参数独立；完整通道 PSD | 两组参数/请求身份；新通道全区间 Welch；141 段平均/最大/最小与短尾段参考对照，含中段及末尾脉冲 | 通过 |
| 更多窗与谱方法 | 7 种窗、周期图/Bartlett/Welch/DPSS/Burg；STFT/DPSS；35 组同系数对照，Bartlett 固定零重叠 | 通过 |
| PSD/STFT 不足补零 | 密度按有效窗能量归一化；观测量、补零量、Δf、ENBW；宽带 3 样本/32 点原生截图 | 通过 |
| 窄带至少 10 帧，原时间轴 | 5 个真实输出样本、10 个补零帧；重复源中心的点击帧 ID 与 PSD 逐 bin 一致 | 通过 |
| 下拉框首行唯一自定义值 | 共享控制器、分隔线、有效输入即时应用、自动适配替换、编辑光标及焦点保持 | 通过 |
| 读入进度、停止、前缀重开 | 实际 4 MiB 顺序读取；样本边界停止；v3 前缀保存/重开与源指纹校验 | 通过 |
| 不超过 2048 点 GPU 导航包络 | 峰值保留合并与真实横坐标；导航滚轮只改变跨度，头尾边界修正 | 通过 |
| 删除驻留保存提示 | 移除持续绘制提示；保留标记选择、限时状态栏及日志反馈 | 通过 |
| 全部勾选框可辨认 | 宽带、窄带、通道弹窗共用灰蓝边框/亮蓝底/白色勾号，悬停、焦点和禁用状态；原生截图复核 | 通过 |
| 既有交互回归 | 无背景反色游标、驻留/帧谱、即时电平、75% 波形、窗口按钮/拖动/缩放、四页与大 IQ 改参取消 | 通过 |
| 切页后的热图与坐标一致 | 目标矩形及右下实际 GPU 像素检查，不只检查绘制调用数 | 通过 |
| 非 CPU QRhi 数据绘制 | D3D11 AMD Radeon 880M，顶点更新、功率纹理上传、数据绘制及完成帧；软件另报 | 通过 |

实现边界见 [架构说明](../../architecture/spectral-settings-and-loading.md)。补零提高网格密度，不增加有效观测时长或真实分辨力。DPSS/Burg 按真实样本校验，参数不足返回原因，不静默换算法。周期图和单帧受资源预算限制；全区间分段 PSD 处理全部分段，不以少量代表帧替代统计。

## 数值结果

[SciPy 1.17.1 对照](scipy-reference.json) 的 FIR/CZT 峰值 bin 一致（154），最大绝对功率误差 `8.61e-10`。35 组估计/窗系数均通过相对 `1e-5` 加绝对 `1e-10` 容差，DPSS 正交参考误差 `4.82e-15`。141 个分段的流式平均/最大/最小谱全部通过同系数一次性参考对照。

Burg 使用独立复数递推参考；**未运行 MATLAB pburg**，不计为 MATLAB 对照完成。[参考输入](reference-fixture.json) 与 `scripts/verify-spectral-reference.py` 可复查本轮计算。

## 原生屏幕、真实加载及资源

全部原生验收仅使用连接序号 **2** 的 **Redmi 27 NU**，原生路径 `\\.\DISPLAY6`，物理 **3840×2160**、DPR **1.5**。主窗口全屏逻辑尺寸为 **2560×1440**；全屏截图为 3840×2160，通道对话框截图为 840×756，使用同一屏实际 DPI。未改变系统屏幕设置或使用主屏替代。

真实 IQ 路径和指纹见原始报告，物理长度 **3,817,472,000 字节**。停止前缀及导航点数取决于用户停止时机，本轮为：

| 配置 | 已读前缀（复样本） | 导航包络点 | 大文件通道首次绘图 | 加载场景工作集峰值 | 加载场景私有内存采样峰值 |
|---|---:|---:|---:|---:|---:|
| Debug | 22020096 | 1344 | 2125 ms | 1313402880 B | 1160454144 B |
| Release | 53477376 | 1632 | 255 ms | 1191452672 B | 1070743552 B |

首次绘图计时从通道创建后的显示请求开始，**不包含此前完整顺序读入耗时**。交互回归场景工作集峰值分别为 1792626688 B、1725906944 B。进程内存包含 Qt/QRhi 资源和映射文件页面，不能当作 IQ 缓存大小；独立缓存计数及采样私有内存在 JSON 中保留。IQ、分析缓存各 128 MiB，单帧工作缓冲上限 64 MiB。

Debug/Release 短窄带场景分别有 58/63 次 GPU 数据绘制、2/2 次纹理上传。两配置都保留 10 帧、选中帧 ID 9；hover 检查确认 FFT/DDC 请求及数据资源上传不增加。

- [Debug 加载报告](debug/spectral-loading-report.json)、[Release 加载报告](release/spectral-loading-report.json)
- [Debug 交互回归](debug/regression/linked-cursor-report.json)、[Release 交互回归](release/regression/linked-cursor-report.json)
- [读入进度](release/01-read-progress.png)、[前缀包络](release/02-prefix-envelope.png)、[宽带补零](release/03-wide-padded-32.png)
- [全通道最大谱](release/04-narrow-whole-maximum.png)、[10 帧补零及驻留帧谱](release/05-narrow-padded-frame.png)、[勾选框弹窗](release/06-channel-config-checkboxes.png)
- [识别页热图与勾选框](release/regression/narrowband-page-2.png)、[大文件通道](release/regression/large-iq-narrowband.png)
- [硬件与软件独立启动报告目录](smoke/)

## 部署与边界

可运行目录为 `out/vs2026-qt611-debug_bin` 和 `out/vs2026-qt611-release_bin`。验证从各自目录运行，PATH 仅保留 Windows 系统目录，清理外部 Qt 环境变量；依靠本目录 DLL、插件、Qt 资源运行。`summary.json` 保存程序 SHA-256 与运行库清单，`artifact-manifest.json` 保存验收文件 SHA-256。

`--narrowband-demo` 使用程序内置 IQ，正常启动仍为工程入口。`prefix-project.json` 是本机真实文件的部分加载验收快照，**不包含 IQ**；跨机器复查需对应源文件。CPU 执行 DSP；星座、眼图同步、识别、解调位流仍为标注的合成演示。Ubuntu、真实模型、同步/解调和持续采集吞吐未在本轮实现或验证。没有已执行而未通过的交付检查。
