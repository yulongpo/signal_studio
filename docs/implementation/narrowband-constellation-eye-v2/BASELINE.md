# 窄带采样、真实星座与眼图 v2：基线

2026-10-22。任务依据 `reference/nb_tasks/PLAN.md` 与 `CODEX_PROMPT.md`，已完整读取。
初始本地和远端 `rebuild/a143-native` 均为 `17471a6488383b456281d12cb1a6d3d80290205d`。
既有未跟踪目录和文件保留，不纳入任务提交。

基线具备真实 DDC/FIR/有理重采样、128 MiB IQ LRU、真实 PSD/STFT。
默认仍是 `min(sourceFs,max(4e6,1.3B))`；调制页星座和眼图为合成示例；同步尚未实现。
v6 保存旧 symbolRate 数字，没有来源信息，不得据此声称自动估计成功。
FIR padding 标记不等于精确有效分析区间，需在 W2 补齐。

所有编译、测试、fixtures 和原生验收从 `./scripts` 进入。
阶段仅定向测试，最终集中 Debug/Release 完整回归。测试阈值固定于 fixtures/index.json，
缓存误差 1e-6、插值 NRMSE 1e-3、Rs 相对误差 1%、CFO 残差 0.001Rs、
定时 RMS 理想/20 dB AWGN 为 0.05/0.10 UI、符号 RMS EVM 为 5%/18%。

本次早期 Qt 探针仅枚举 S2719DGF / DISPLAY10，Windows 另有 Redmi 27 NU 和
GameViewer 虚拟显示适配器。用户已明确保持 Redmi 27 NU 实屏 4K/150% 验收；
W8 必须重新探测，不得以当前虚拟/缩放显示替代。

W0 实测：`./scripts/verify-narrowband-analysis.ps1 -Node W0 -Configuration Debug -GenerateFixtures`
生成 25 组 CI16/CF32 文件与独立发射真值；重建后的 `signal_studio_channel_processor`
通过（1/1，1.46 秒）。这只证明既有 DDC 基线，不代表新增星座/同步能力。
日志：`out/tasks/wideband-detection/NB-W0/logs/baseline.txt`。
