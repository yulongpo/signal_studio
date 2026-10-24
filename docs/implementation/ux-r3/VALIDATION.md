# UX-R3 验证记录

本记录冻结独立终审前的本地验证事实。完整 Debug/Release 回归、实际实屏与独立部署已完成；历史失败逐轮保留。独立审核与远端发布状态另见 INDEPENDENT_REVIEW.md 和 POST_REVIEW_SYNC.json，不以本地测试代替审核。

| 轮次 | 实际范围与结果 |
| --- | --- |
| G0 初步 Debug 构建 | 工程树目标构建成功，CTest 1/1；修改前 Release 实屏截图为基线部署证据 |
| 首轮功能定向 | 轴测试通过；标签 2 处发布边界错误、工程文件夹具指纹缺失等失败已记录在 `G0/logs/feature-tests-debug-v1.txt` |
| G2 修复后 | 原断言保留，工程文件行为 33/33，CTest 1/1；包含复制/引用另存、取消、无写权限、迁移、损坏资产及错误重定位 |
| G4 定向 | 已执行可信 profile、映射及来源证据边界；最终首轮全量的标签 QtTest 22/22 |
| Debug 全量构建 v1 | `G7/logs/Debug-full-20261023-203438-081.txt`：capture 适配中的 if/else 变量重定义导致编译失败，已修复，未将其计为测试通过 |
| Debug 全量 v2 | `G7/logs/Debug-full-20261023-203805-321.txt`：33/40，7 个 CTest 失败，包含参数组 480 s 保护终止及 annotations 中 Qt 函数超时；总用时 1226.28 s |
| Debug 工作流恢复 v1（2026-10-24） | `G7/logs/debug-recovery-workflows-20261024-v1.txt`：14/15 Qt 行，失败已推进至保存后弹层重新打开；原输入草稿不误报修改断言已通过 |
| 跨盘引用恢复 v3 | `G7/logs/debug-cross-drive-regression-20261024-v3.txt`：工程文件、工作流、窄带 UI、检测交互、布局共 5/5 CTest；另 6/6 UI 定向 Qt 行通过，包含真实 TorchSig 保存重开 |
| Debug 全量 v3 | `G7/logs/Debug-full-20261024-225331-473.txt`：新增检测保护用例缺少 QMessageBox include 导致编译失败；补齐后重新构建 |
| Debug 全量 v4 | `G7/logs/Debug-full-20261024-225633-210.txt`：36/40；标签预算用例先触发 5 s deadline，任务警告后焦点、辅助 Y 轴旧坐标失败，参数组触发 480.04 s 保护；总 1144.59 s |
| Release 全量 v1 | `G7/logs/release-full-20261024-v1.txt`：37/40；任务焦点、辅助 Y 轴和 annotation 文件菜单入口适配失败。窄带此轮未设 W8，不能充当最终 W8 证据 |
| 修复定向 | `G7/logs/debug-budget-and-legacy-fixes.txt`：标签及工程文件 CTest 2/2（含 35 个工程用例）；`active-save-as-focus-v2` 3/3 Qt 行，`ui-axis-and-open-action-v2` 4/4；未放宽业务断言 |
| Release 完整实施版 | `G7/final-release/logs/Release-full-20261024-231911-583.txt`：实施 HEAD `3499cfb`，40/40，291.08 s；行覆盖 106 个主 UI、11 个窄带业务行，W8、0 skipped |
| Release 映射复用修复版 | `G7/final-release-717ab8f/logs/Release-full-20261024-232823-022.txt`：HEAD `717ab8f`，40/40，308.44 s；再次核对主 UI 106、窄带 11 行和 W8，0 skipped |
| 最终冻结 Debug 全量 | `G7/final-debug-frozen/logs/Debug-full-20261024-235432-890.txt`：HEAD `064b509`，40/40、1400.40 s；主 UI 三组 175.72/426.05/198.27 s，TIMEOUT 各 480；106 个主 UI 和 11 个窄带业务行全部覆盖，W8、0 skipped |
| Debug 过渡集成轮 | `G7/logs/debug-final-3499cfb.txt`：39/40、1603.44 s；唯一失败为参数组 480.05 s 保护终止。当轮与 Debug 原生矩阵并行，且构建后仍追加了映射复用检查，不把它称为冻结最终版或全过 |

Release 完整实施版之后，交付前核查又收紧 TXT 映射复用为精确标签路径、相同 IQ 绑定和唯一解释。该修复的首次定向编译因断言插入位置引用了其它用例的变量失败，日志 `G7/logs/release-mapping-reuse-binding.txt` 保留；位置修正后 `release-mapping-reuse-binding-v2.txt` 的标签 CTest 1/1、22/22 Qt 行通过，随后执行上表 `717ab8f` 全量。

## 原生矩阵

轴 A/B/C × 波形/PSD × 时频/瀑布 × 长短刻度：Debug GPU、Debug 软件、Release GPU、Release 软件各 **24/24**。每场保留实际 4K PNG、文字 ink 边界、主/辅助 plotRect 对齐、物理游标往返、X/Y 滚轮/拖动、ROI 取消与标记角点/撤销断言。详见 AXIS_LAYOUT 的实测成本及 `G7/native-axis-*/diagnostics/` 的逐场景 JSON。

早期 Debug 硬件尝试 `G7/native-axis-debug/` 在短波形采用已处于最小合法跨度的 [-1,1] 时失败，导致进一步缩放被正确钳制；主 Agent 在保存 4 场检查点和 `INTERRUPTION.json` 后停止了该专属进程。夹具改为 [-16,16] 后重新执行完整矩阵，成功路径为 `G7/native-axis-debug-v2/`。原失败和中断不计作成功场景，不删除旧证据。

Debug/Release 窄带指定实屏 GPU/软件各 **18/18**，共 72 场景，使用 CI16 RAW 与 CF32 SigMF IQ 文件，经实际 CPU DDC 和恢复链得到星座/眼图；覆盖原始/手工 Rs/恢复、真实模型不可用、眼图保存重开与取消。Release 检测指定实屏 **46/46**，包含实际 TensorRT 模型推理、结果复核/来源修订/CPU DDC、ROI、保存重开、真实模型取消前缀和明确标示的十万合成活动段压力场景。

## 独立部署与关联

Release 部署六组合（宽带、窄带五页、保存的真实眼图工程 × GPU/软件）均通过，见 `G7/diagnostics/deployment-release-v2/deployment-summary.json`。从对应 `_bin` 目录启动，仅系统 PATH，清空 Qt/QML 外部插件路径；不依赖外部 Qt 安装目录。`real-eye-project-gpu.json` 的活动页为 4、热图上传 0、实际顶点上传为正，真实眼图 256 条；软件报告的 GPU 计数为零。

首次五页 GPU 包验证失败保留在 `G7/diagnostics/deployment-release/pages-True-software-False.json`：Rs 未确认，因此眼图按产品规则为空，报告只完成四页，未伪称五页。验收脚本现在通过专用 CLI 明确设置 **1 MHz 手工 Rs 假设**，并将其写入 `acceptanceSymbolRateHypothesisHz`；该值仅为内置样例的图形验收前置条件，不是估计结果或真实调制识别。普通应用启动及未知 Rs 的产品默认行为保持。

Windows 关联控制测试实际注册 HKCU 本程序 ProgID，通过 ShellExecute 打开带空格和中文路径的空 `.ssproj`，验证部署 exe、`--project` 参数和工程标题，最后关闭专属进程并注销本程序关联，`association-v2.json` 为 pass、unregistered=true。首轮注销使用只读注册表句柄失败，改为显式 writable OpenSubKey 后复验；错误转录单独标注为 `G7/diagnostics/association-initial-failure.json`，不伪造程序产生的报告。其它应用的默认关联不被覆盖。

原生矩阵使用集成构建的 UiCapture，原报告不补写不存在的最终 HEAD。后续提交收紧 G4 映射复用以及 CLI/文件关联验收，G3 轴布局、G5 绘图/DSP 和 G6 worker 源码未改变。最终完整 Debug 的源码冻结为 `064b509`，此后仅 `app/main.cpp` 启动验收入口变化，CTest 的测试/业务源码均未改变；最后代码提交为 `05d80d5`。版本范围、目标输入差异和二进制身份明确区分，独立 Review 审查的精确 HEAD 还包含交付文档与证据提交。

最终部署补测中，Debug 保存眼图的固定 150 ms 覆盖层检查出现失败，报告中硬件、恢复、顶点与 4K 均成立；失败原件保留在 `deployment-debug-final-064b509/real-eye-project-gpu.json`。启动验收改为至多 5 s 等待真实绘制完成及 GPU 新帧，并修正异步计数/JSON 状态在事件循环期间的生命周期。`05d80d5` 在 Debug/Release 重建后，宽带/五页/真实眼图 × GPU/软件 **12/12** 通过，最终目录分别为 `G7/diagnostics/deployment-debug-final-state-v3/`、`deployment-release-final-state-v3/`。没有改变业务 DSP、页布局、CTest 断言或 480 s 保护；相关 CLI 改动由这 12 个实际进程验证，而非将较早全量结果冒称新 CLI 的验证。

首轮全量失败项：`signal_studio_ux_r3_workflows`、`signal_studio_narrowband_analysis_ui`、`signal_studio_ux_detection`、`signal_studio_ux_layout`、`signal_studio_ui`、`signal_studio_ui_parameters`、`signal_studio_ui_annotations`。原始完整日志为证据，不以其中任意定向成功覆盖整轮结论。

已实施的相关修复：自动标签发现改为可取消的静默后台工作，发布时再次核对会话/源绑定；自动幅度拟合的派生边界不计为用户编辑；IO 返回前恢复原窗口焦点并完成队列中的弹层转换；新轴几何测试沿实际 plotRect 命中与分配空间核验。窄带眼图三个编辑器缺失 QObject 父级的异常由 PDB 指向和 0xc0000005 记录定位，修复与定向运行另见 `G5/diagnostics/20261024-nb-owned-editors/`，不冒称整轮通过。

2026-10-24 当前实屏探针：Redmi 27 NU、连接序号 **1**、`\\.\DISPLAY6`、主屏，首选/实际 **3840×2160**，DPR **1.5**，逻辑 **2560×1440**；报告在 `G7/probe-20261024/diagnostics/`。这与 G0 连接序号 2 的历史探针分别记录，不按连接序号推断屏幕身份。

本地结果：Debug/Release 完整 CTest 各 40/40；两配置主 UI 106、窄带 11 行无遗漏/无跳过；轴矩阵 96 场景、窄带矩阵 72 场景、Release 真实模型检测闭环 46 场景；最终二进制部署 12/12，系统文件关联打开/注销通过。下一硬门禁为最终精确 HEAD 的独立 Review；只有 APPROVED 且待推送 HEAD 完全一致、最新远端安全可同步时才可推送。
