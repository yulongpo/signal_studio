# 窄带采样与真实星座／眼图 v2 交付

W0–W8 的开发、真实文件链路、数值与必要验收已闭环。默认目标 2B，1.5/2/4/8 倍率及独立手工 Fs，受 FIR/L/M/源 Fs/资源限制的实际速率规划，连续 IQ 星座和非整数 SPS 眼图，Rs 候选与 BPSK/QPSK/16QAM 的 CFO/RRC/Gardner/载波恢复均已接入正式 Qt 调制页。v6 缺字段保持历史实际 Fs、快照和未确认旧 Rs，不保存 IQ 大数组。

开发分支 `feature/narrowband-sampling-constellation-eye`，基线 `17471a6488383b456281d12cb1a6d3d80290205d`。W0–W7 本地节点为 7348ec0、2f6ed65、64feedd、a620119、cdb9e4f、5e22c78、f8d55b6、7d658a4。W8 完成技术验收后，经 DSP、Qt 专项复审及独立正式终审通过才本地提交，不推送/合并远端。最终身份在提交后由脚本单次写入 [FINAL_GIT_IDENTITY.txt](../../../out/tasks/wideband-detection/NB-W8/diagnostics/FINAL_GIT_IDENTITY.txt)；文档不内嵌自身提交 ID。原有未跟踪内容保留。

Debug/Release 完整 35 项回归均执行；首轮各 34/35，唯一旧布局用例的字体/宽度前提修复后定向通过，最终全部 35 个必要项均有通过证据。主 UI 原 106 个业务 data 行按三组执行，含 init/cleanup 共 112 PASS；新增窄带 9 个业务行、共 11 PASS。所有行集合严格核对，无遗漏/重复，各组仍为 480 秒保护。详见 [验证原文与失败闭环](VALIDATION.md)及 [汇总](evidence/SUMMARY.json)，不将补测改写为首轮全过。

指定 Redmi 27 NU / 连接序号 2 / DISPLAY6，物理与首选 3840×2160、150% DPI、2560×1440 全屏。两配置 GPU/软件各 12 场景，共 48 场景；两配置实际工程 GPU/软件独立启动四种模式均通过，仅系统 PATH，Qt/CRT 从输出目录加载。GPU 与软件截图已查看；API 夹具设置与 QPA 控件操作、生成 IQ 与现场 RF 的边界见 VALIDATION.md。

固定数值门槛全部保持：缓存/直接 IQ 最大差 1.33183e-14；解析带限眼图 NRMSE 0.000169596；24 个 Rs 正例全部命中前三候选 1%；13 个同步正例最大 CFO/Rs 8.9262605e-6、真值定时 RMS 0.0293794 UI、真符号 EVM 4.64409%。负例不报告可信载波锁定，真实 raw 保留。

程序位于 `out/vs2026-qt611-release_bin/SignalStudio.exe`，Debug 对应 `out/vs2026-qt611-debug_bin/SignalStudio.exe`。启动：

```powershell
./scripts/run.ps1 -Configuration Release
```

步骤见 [使用说明](USER_GUIDE.md)，完整复验及生成数据、原生捕获、独立部署命令见 [手动复验](MANUAL_VALIDATION.md)。构建、测试、数据生成及原生验收始终由 scripts 下脚本执行。没有建立源码、数据、截图或日志 SHA 清单；既有缓存身份、工程完整性、QSaveFile、Qt/CRT 部署校验保留。

已实现能力限于所列单载波分析假设和规定样本。现场 RF 准确率、任意未知制式自动识别、解调位流、设备持续采集、Ubuntu、跨 GPU 部署和峰值内存基准没有新增通过声明；深度学习识别和解调演示仍明确标示合成。代码审查与最终提交门槛见 [REVIEW.md](REVIEW.md)。
