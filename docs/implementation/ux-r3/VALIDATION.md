# UX-R3 验证记录

本记录保留首次独立终审前的本地验证事实及后续修复轮次。首次终审结论为 REJECTED（3 Major），修复后的构建、行为和实屏验证见末节新记录；旧完整 Debug/Release 回归、实际实屏与独立部署不豁免新缺口。历史失败逐轮保留。独立审核与远端发布状态另见 INDEPENDENT_REVIEW.md 和 POST_REVIEW_SYNC.json，不以本地测试代替审核。

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

## 首次终审及披露修正（2026-10-25）

独立审核 df0ba04 为 REJECTED：M1 活动检测索引变化使旧容器摘要失效；M2 Reference 普通保存切换资产根；M3 判决观察、参考区域和 I/Q 读数缺失。原报告完整保存在 out/tasks/wideband-detection/UX-R3/review/REJECTED-df0ba04.md；修复后必须审核新精确 HEAD。

当轮 Release 原生启动反馈 367.1684 ms（目标未达），取消可见 123.8201 ms、取消完成 278.1047 ms；十万合成活动段筛选 p95 416.1672 ms、选择 p95 435.4769 ms，均未达 200 ms 建议目标。这里披露限制，不将建议目标变为额外硬门禁，也不声称未剖析的性能原因。

独立纯数据区重算：A/B 主图 -2.904% 至 -1.708%、辅助图 -28.56% 至 -27.68%；C 主图 -3.116% 至 -1.896%、辅助图 -17.367% 至 -16.327%。外部分配测试不是纯数据高度下限，逐场景 plotRect、部件尺寸及文字带预算保留在原生 JSON。

## 独立拒绝后的修复验证（代码 bc22bd5）

三项 Major 的代码修复已本地提交，首次规范拒绝报告逐字节归档为 `evidence/review-fixes/initial-independent-review-df0ba04.md`，原独立复现日志也保留。独立 Review Agent 的预审读取实际新代码并复算判决/位置/鼠标坐标，未确认原 Major 仍存在；预审不是最终 APPROVED，仍等待最终文档和验证后的精确 HEAD。

| 修复轮次 | 实际结果与范围 |
|---|---|
| codec 首次新增回归 | 45/46；测试在 save 替换 Project 后继续使用旧 run 引用，修正夹具引用生命周期；原失败归档 `codec-first-45-of-46.txt` |
| Debug / Release 定向 | 各 3/3 CTest：工程 46/46、调制族数值、窄带 UI 14 Qt 行/12 业务行；该次未设 W8，不充当 W8 最终证据 |
| Reference 活动保存 UI 首次 | terminal-b 旧历史未设置合法 contextRange，真实 writeDetectionRun 拒绝；补齐第二源上下文后重跑，没有删除或放宽旧断言 |
| Reference 活动保存 UI Release | 3/3 Qt 行含 init/cleanup，实际 QAction → QFileDialog → No 引用，普通保存保持根/reference，继续增加分片，取消后不重存重开最新前缀；闲置普通保存仍保留引用 |
| Debug 完整构建首次 | capture 仍运行时链接报 LNK1168，未进入 CTest；等待专属原生进程自然结束后原样重建，未杀用户进程 |
| Release 完整修复版 | 40/40 CTest，295.36 s；主 UI 106、窄带 12 业务行，W8、0 skipped，逐行并集核对通过；三个主 UI TIMEOUT 仍各 480 |
| Debug 完整修复版 | 40/40 CTest，1589.32 s；主 UI 106、窄带 12 业务行，W8、0 skipped，逐行并集核对通过；主 UI 三组 299.13/417.62/194.75 s，TIMEOUT 各 480 未变 |
| 新窄带实屏矩阵 | Debug/Release × GPU/软件各 26/26，共 104 场景，旧 18 及新增 8 完整；对应四份 `evidence/review-fixes/native-*.json` |
| 真实模型保存恢复扩展 | Release 46/46，真正 TensorRT 任务 Running 时保存，再实际推进，取消后旧容器字节不变也能加载最新 Cancelled 及完全一致的持久前缀/冻结请求；不是用 API 夹具冒充模型运行 |

新原生四组合与真实模型仍使用 Redmi 27 NU、连接 1、DISPLAY6、3840×2160、150%。M3 的参考区域为 QPainter 矢量覆盖层，真实散点仍为 QRhi 顶点；软件 GPU 计数为零。API 子进程中断回归只终止 QProcess 自建夹具，不冒称 TensorRT 崩溃验收。Debug 窄带矩阵采集在额外 G6 capture 断言之前，M3 路径源码相同；此后的差异仅增加 wideband capture 的活动保存恢复断言，不改变窄带实现或场景。

新 Release 实屏启动反馈 172.3765 ms、取消可见 67.3124 ms，二者本次达到 200 ms 建议值；取消完成 224.0829 ms。十万合成活动段筛选 p95 415.1766 ms、选择 p95 425.5413 ms，仍未达 200 ms。保留首次 367.1684/416.1672/435.4769 ms 未达快照；不以单次快照宣称稳定性能通过。复验期间另有 Debug CTest 在运行，没有做独占负载或瓶颈因果剖析。

原始完整日志、当前逐行覆盖、26 场景/新模型报告及最终部署版本映射归档到 evidence/review-fixes/，旧 71 项归档逐字节保留；最终独立报告必须另行核对完整精确 HEAD。
CMake 在配置时将 Git 短修订写入 Brand 的 About 元数据；修复构建开始于代码提交之前，首轮全量二进制带前一修订标识，实际业务源码与 bc22bd5 相同。全量结束后再次配置并仅重建 app/capture/worker，使交付版本标识为 bc22bd5；业务源码不变，最终 12 个实际部署进程按新二进制重新验证，六个 exe 身份以 final-binary-identities-bc22bd5.json 为准。About 元数据变化不冒称重新跑过业务全量，构建转录与前后证据分别保留。

最终交付版本元数据对齐 bc22bd5 后，Debug/Release 的宽带、五页、真实眼图 × GPU/软件共 12/12 再次通过，均从对应 _bin 目录、仅系统 PATH 独立启动；app/capture/worker 六个最终二进制 SHA-256 已记录。当前 ARCHIVE_MANIFEST 为 151 项，原 71 项 bytes/SHA 全部保持，新增证据与关键截图逐字节归档；规范最终报告在最终提交后由独立 Reviewer 生成，不由主 Agent 签署。
