# UX-R3 FEEDBACK-01 独立工作区审核

Decision: APPROVED
Scope: LOCAL WORKTREE SNAPSHOT ONLY
Base-HEAD: 1608c4c87dd102fbe21e01e0d83c5e606119d50e
Branch: feature/ux-r3-optimization
Source-Content-ID: 698760c846b05b1cd2489bec1a3024de890c416ad0443ff53c91606bf4b7c745

审核日期：2026-10-25（Asia/Shanghai）。本结论批准下列精确未提交源码快照的本地交付；不批准任何 release HEAD，不授权提交、推送、分支同步或发布。Base-HEAD 仅标识差异基线，不代表该提交包含本次反馈修复。后续任何代码、测试或验证脚本变化均不属于本次批准对象。

本次范围内没有尚未解决的代码发现或必要验证阻塞。独立审查发现的 RF 极窄范围刻度精度问题已经闭合；主任务随后发现的真实程序启动默认值覆盖也已独立复查并用最终启动证据闭合。1600×900 的历史等待失败仍按范围外未查明现象保留，不宣称修复。

## 审查独立性和范围

审核者未参与实现或自行修复代码。整个审核仅执行只读源码、Git 状态、日志、JSON、PNG 和哈希核对；未启动构建或 GUI 测试，未提交、推送、切换分支或执行发布脚本。唯一写入为本报告。旧报告 `docs/implementation/ux-r3/INDEPENDENT_REVIEW.md` 保持原样，其 SHA-256 在审核开始和结束核对均为 `d65fa9319a490fab0fd532e1f9401c844afbd77f7d5d4d2f35462e38bb9957ab`。

读取了项目 AGENTS.md、全部本轮工作区差异，以及未跟踪新增的 `ui/charts/axis_description.h`、`tests/ux_r3_label_capture.h`；复查了原有导入发布、坐标映射、缓存、启动和控件溢出路径。差异中的两份历史说明文档及本轮 DELIVERY、VALIDATION、SOURCE_SNAPSHOT、BINARIES、ARCHIVE_MANIFEST 也已核对。用户原有未跟踪资产不作为本轮实现变化或新验收结果。

重点审查内容如下：

- WAV/DATA 与 TXT 的正向匹配和反向定位、同名多源和大小写冲突、未知 TXT 字段映射、受限预览和取消、WAV RIFF 数据边界、源身份和去重。
- 文件菜单及底部按钮共用手工导入路径；目标信号可见、确认前不发布、取消和上下文切换中止、原子来源集合发布及已有人工修订保留。
- CompactHeader 默认值与 A/B/C 原枚举保持；标题栏完整/短文本、单位和原点、悬停和更多设置、主辅图左缘预算及真实 plotRect。
- 五个窄带页面、十个图控件、六类图形的动态刻度墨迹、等比例星座、热图定位、游标逆映射、字体/DPR 变化、分析结果和缓存复用。
- `app/main.cpp` 的真实启动默认模式，以及 `scripts/verify-package.ps1` 对默认模式和显式 A/B/C 的独立启动断言。

## 已闭合的发现与失败边界

### RF 极窄范围刻度精度

原 `tickText()` 在步长很小时固定使用 12 位有效数字，RF=102 MHz、基带范围 ±0.0005 Hz 可产生重复频率文字。最终实现按数值量级和步长确定有效数字，上限 17；布局同时将相邻重复文字纳入刻度稀疏判断。新增 `compactRfTicksKeepNarrowRangePrecision` 对 PSD/STFT 分别检查唯一刻度及回读误差小于步长的 0.1 倍，Debug/Release 最终窄带 UI 均通过。此发现已解决。

### 实际程序启动覆盖 CompactHeader

原入口的 `--axis-title-mode` 默认 A 且无条件应用，覆盖窗口内部的 CompactHeader。最终入口默认 `compact`，保留显式 A/B/C，并分别映射原枚举。部署验证默认不传此参数，直接检查主图及辅助图实际报告均为 CompactHeader；显式模式另行验证。最终 8 份 `package-final-*` 和 6 份 CLI 报告均已核对，首次模式仍为 A 的旧启动报告不用于闭合默认模式要求。此发现已解决。

该最后修正只涉及应用入口和部署脚本，重新构建了 Debug/Release 的 SignalStudio，并重跑 8+6 次启动。CTest 和原生矩阵使用的共享 UI、DSP、测试和捕获实现未再修改，现存二进制哈希与对应清单一致；不冒称这些矩阵全部是在入口修正之后重新运行。

### 测试适配的有效性

六处窄带旧固定边距已替换为实际 plotRect，原有绘制覆盖、锚点误差、轴命中、热图和游标断言保留；新增独立字体墨迹、画布边界及面积检查。

DPR 1.5 的 QWidget QTest 鼠标请求 `(776,520)` 曾实际投递为 `(776.333,520)`。最终测试先约束实际事件距请求点不超过 1.1 逻辑像素，再使用实际事件坐标保留 1e-9 的 I/Q 逆映射阈值，没有放宽生产映射精度。色表测试验证稳定 owner、所属 strip 或其弹层祖先、唯一编辑器和刷新前后父级一致，符合既有响应式溢出实现。

旧色表测试在整组运行时以 1600×900 停留于“等待交互停止后计算”；改用项目规定的 canonical 2560×1440 逻辑窗口后完整 geometry 组通过，15 秒 DDC 就绪断言未放宽。Windows 原生 helper 另外检查实际 4K/150% 全屏；offscreen 的尺寸测试不被当作硬件显示证明。原 1600×900 失败仍未查明，不宣称调度器或该尺寸已经通过。

新增测试曾因临时 QJsonValueRef 生命周期出错而崩溃，已改为持有 QJsonValue；失败的调用参数、初始化后截断日志、堆栈记录边界和链接占用等按 DELIVERY/CRASH_ANALYSIS 留存，不与生产崩溃或一次全过混同。

## 最终验证核对

以 [VALIDATION.json](VALIDATION.json) 定位报告，同时读取原始证据，不只依据汇总的 pass 字段。

| 验证范围 | 核对结果 |
| --- | --- |
| Debug 相关 CTest | 单轮 14/14，928.00s |
| Release 相同集合 | 14 项均有通过证据：serial-r2 的 13 项加 canonical geometry 补测 1 项；保留原 13/14 失败日志 |
| canonical geometry 补测 | Qt 31/31，CTest 32.02s |
| 主 UI 三组 | Qt totals 为 31、58、23；扣除各组 init/cleanup 后为原有 106 个业务 data 行 |
| 窄带 UI / 轴 / 工作流 | Debug/Release 分别为 Qt 17/17、17/17、27/27 |
| 标签真实 golden | MATLAB WAV/TXT 10 对象、65 段；TorchSig 4 标签；源 hash、RIFF 边界及绑定证据保留 |
| 四组原生矩阵 | Debug/Release × GPU/软件回退，每组 32 宽带 + 26 窄带 + 6 标签入口，合计 256 个场景/入口检查 |
| 最终独立启动 | 8/8；不传轴模式时主辅图均为 CompactHeader，system-only PATH，GPU/软件及宽带/窄带分别记录 |
| 显式 CLI 兼容 | Debug/Release × A/B/C，6/6，主辅图实际模式均匹配 |

CTest 原始通过行与 VALIDATION 的每个集合条目逐项一致。直接核对的主要日志为 [Debug 最终整组](evidence/logs/test-debug-serial-final-complete.txt)、[Release serial-r2](evidence/logs/test-release-serial-r2.txt) 和 [canonical geometry 补测](evidence/logs/test-release-geometry-canonical.txt)。这是本次相关 14 项集合，不声明仓库全部 CTest 已重新通过。

四组原生 JSON 中的每个宽带场景、轴布局布尔检查、窄带 compactAxes 边界/墨迹/标题/映射字段均通过；窄带各组覆盖十个图控件和六类图形。24 个标签入口场景均无超时或失败清理，状态 token、标签和 sourceSets 保持预期，底部面板状态也恢复。辅助图同尺寸比较至少增加 39 逻辑像素，面积至少增加 44.318%；对比未通过增加标题栏高度取得。

本轮实际屏幕为 Redmi 27 NU，连接序号 1，`\\.\DISPLAY6`；Windows DisplayConfig monitorDevicePath 为 `\\?\DISPLAY#XMIB005#5&326bf1a6&0&UID409856#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}`，物理 3840×2160、逻辑 2560×1440、DPR 1.5。GPU 报告记录 QRhi D3D11 / NVIDIA GeForce RTX 5060 Laptop GPU 及实际上传、绘制证据；软件报告明确为 QPainter。已人工查看 Release GPU 宽带 CompactHeader/窄带 DDC、Debug 软件恢复眼图及实际标签预览 PNG，未见与报告相矛盾的布局或来源标识。

## 身份和证据完整性

最终核验结果：

- 下表 21/21 源码、测试和验证脚本大小及 SHA-256 与磁盘一致，覆盖当前全部本轮代码写集，包括两份新增头文件，无漏项。
- [ARCHIVE_MANIFEST.json](ARCHIVE_MANIFEST.json) 的 193/193 文件大小和哈希一致，归档目录无清单外或缺失文件。
- 四份全量 PNG 清单中的 288/288 原始 PNG 存在且大小、哈希一致；归档复制代表图，完整原图仍位于任务目录，二者未混称。
- [BINARIES.json](BINARIES.json) 的 14/14 二进制大小和哈希与部署目录一致。

内容 ID 使用清单生成时的 PowerShell `Sort-Object path` 顺序，将 `path:lowercase-sha256` 以 LF 连接且无尾 LF 重算，匹配本报告 Source-Content-ID；不要用不同的 ordinal 排序结果冒称文件漂移。

| 索引 | SHA-256 |
| --- | --- |
| SOURCE_SNAPSHOT.json | `0b70aa002924a24e6781e5dcad5346a56c24bb182e70c3edd290595c0c6a3b69` |
| VALIDATION.json | `b5cf5876bd6fad4ab8a3ad33afe93e3dc3ae4a6583637ac50c31eef54c933e47` |
| ARCHIVE_MANIFEST.json | `58c5a2179211196496b11d6174db1e75e56ae9c81bdd980384b8edfc5f463f1c` |
| BINARIES.json | `9f0e917f825558225438978119c32165f0d8cac91c47aa791be4deb7a174b638` |

### 精确未提交文件清单

路径相对本仓库；哈希针对当前实际文件字节，包括其现有行尾。

| 文件 | 字节数 | SHA-256 |
| --- | ---: | --- |
| `app/main_window.cpp` | 255782 | `a209e4c1013c15c2028c553ac06ff2fc0110549e916e11f465b79cb1afdd2819` |
| `app/main.cpp` | 29696 | `d94000035eba493fbf09c898ee1006ad716e1f93f9ee91a688a98261e7044789` |
| `infrastructure/label_discovery.cpp` | 19784 | `93a03f3bf5d15f89835b41535e957e3083f8c04c240c6667d6fa0884b20cd357` |
| `infrastructure/label_discovery.h` | 3788 | `f8ef18412efe5228aecc604be5beef62db0e246228ff24d1d01a1d3046f74616` |
| `scripts/verify-package.ps1` | 6366 | `dbd5a37d3836c84225e4bc219a926f11e20b82610560657ecc0fdb799fc26776` |
| `tests/label_import_tests.cpp` | 66668 | `191935e396f2bed4cd439a7c5ff9ef81e8b00534ee11010bf6688262532f8861` |
| `tests/narrowband_analysis_capture.h` | 32980 | `2aa8cd19e3789473a72bddb082d5b80882f2c258019ff98bc1aa650f34c13cad` |
| `tests/narrowband_analysis_ui_tests.cpp` | 57663 | `4c5183f08f554e81dc0a5bf4a6fb2426f3607643cc5641301c7e24a820e82780` |
| `tests/ui_capture.cpp` | 60179 | `1d6b61534772ac20375137b22f659a7039625033d6a922542479f890cf864777` |
| `tests/ui_tests.cpp` | 255198 | `7eab2e6a449a05c584401987958cf03f52b045592fb4c84b581505a6fff27640` |
| `tests/ux_r3_axis_tests.cpp` | 10910 | `f4f767713e9f78c73b243c8d2dd74a6a1c24acc6147748c25a84bfe61260b553` |
| `tests/ux_r3_capture.h` | 46303 | `3723e571333bbe2628e77548a6ab4ff8bc09da5d52ddaff7793d8226ef2931ff` |
| `tests/ux_r3_label_capture.h` | 26190 | `f4c378126d1d6e2af6a8b685c0cde962de48ad9d1062ef98bf3cbff711cf07b0` |
| `tests/ux_r3_workflow_tests.cpp` | 30238 | `bf35e7af7514be96910cbf3517cf530229098f647c70b96322ac7996cf271810` |
| `ui/annotations/annotation_panel.cpp` | 80839 | `8a51883e68f87f732fe088ca0f8d01b33bcfbad77caac0faaca978d89cef0a36` |
| `ui/annotations/annotation_panel.h` | 3190 | `915cba524c45dbb4d8e9ffe2f2dfa010295d18d1caac6c9a97af9c8e85901c2c` |
| `ui/charts/axis_description.h` | 3291 | `9025a3e182b18a668147560ab344ec4d604ee171e4872ede404b9804dfed99be` |
| `ui/charts/axis_layout.h` | 4105 | `659b4bba11f233d4040406bf9bdcd99b7ecc9ce8124608ec39ad243cf6a37c5c` |
| `ui/charts/plot_widget.cpp` | 166223 | `cc19b0a8ec5e2960c2842b12f1699d9507bbb09fcb1e0e3f8cb9702e63a6fa02` |
| `ui/charts/plot_widget.h` | 9948 | `934ea7effb67df7c53f5a0224269d12cb3e2c19e4bdbc0cb09c37ab1182e5010` |
| `ui/narrowband_workspace.cpp` | 222085 | `75d9bd2d57fcfdbad79adb6458d480da67bde37230e5f5a545e9819ebb46da54` |

## 验证边界

标签导入中四个文件系统符号链接用例因本机无法创建链接而明确跳过，未将其记为通过。原生标签文件选择器证明实际 Windows Qt QFileDialog 的可见输入与业务流程，不证明 Windows Shell picker。眼图和星座来源按报告中的真实 IQ/恢复假设解释，识别模型及合成解调示例仍保留原有标识；本轮不新增真实 RF 精度、设备输入、推理模型、真实位流或 Ubuntu 已通过声明。

本批准仅适用于上述本地未提交快照和当前验收范围。任何后续发布仍须按项目门禁对最终新提交及精确 HEAD 重新独立审核，本报告不能替代该审核。

