# UX-R3 FEEDBACK-01 最终提交独立发布审核

Decision: APPROVED
Reviewed-HEAD: 1d809b5a61a6fdfc52d648c152d5292e09392774
Reviewed-Tree: 85d6a4947792da01dbcd0564591b90fe6324e1bf
Base-HEAD: 1608c4c87dd102fbe21e01e0d83c5e606119d50e
Branch: feature/ux-r3-optimization
Scope: FINAL COMMITTED RELEASE HEAD
Validated-Source-Content-ID: 698760c846b05b1cd2489bec1a3024de890c416ad0443ff53c91606bf4b7c745

审核日期：2026-10-25，Asia/Shanghai。用户已明确授权本轮提交并推送。独立审核对上述精确最终提交给出明确批准，本次范围内没有未解决的代码发现、验证阻塞或发布审核缺项。这是新的提交后审核结论；此前本地未提交快照报告不单独构成本次发布批准。

本报告在被审核提交生成后写入本地 `release/INDEPENDENT_REVIEW.md`，必须保持未跟踪且不再提交，避免改变审核身份。审核者未实施修复，未修改跟踪文件或旧报告，未构建、重跑 GUI、提交、推送或 fetch；仅做只读 Git/远端查询、源码和证据审查、哈希核验，并新增本报告。发布操作由主任务按用户授权执行。

## 精确提交与已验证实现的绑定

审核开始及写报告前，HEAD 均为本报告标识的最终提交；其唯一父提交为 Base-HEAD。跟踪工作区和索引均干净。用户原有 `reference/`、`resources/sigmf_data_demo/`、`tasks/`、旧宽带文档、`.vscode/`、zip 及根目录旧审核附件等仍为未跟踪内容，没有混入本次提交。

读取了本次 `git show`、父子提交差异、提交树和发布资料。本提交共 225 个路径：21 份源码/测试/验证脚本、193 份归档证据及 11 份说明/索引/属性资料；未发现额外代码、用户资产或范围外路径。已验证代码与前一轮逐文件独立审查的冻结快照一致，追加的 DELIVERY 发布授权、长路径说明及局部 .gitattributes 未引入业务实现变化。

独立核验并未只采用主任务的 COMMIT_BINDING 汇总：

1. 逐一重算 21 份工作区文件的 SHA-256/字节数，全部匹配 SOURCE_SNAPSHOT。该历史清单记录本机验证时的原始字节，内容 ID 没有变化。
2. 对每份文件用 `git hash-object --path=<path> --stdin` 计算 Git clean/行尾规范化后的对象身份，与当前提交中的 blob OID 逐项一致。另直接读取提交 blob 比较：8 份原始字节完全一致，13 份仅 CRLF→LF，没有其它字节内容变化。没有针对这些源码的新自定义 filter。
3. 直接读取提交中全部 193 份证据 blob，逐项检查大小及 SHA-256，全部匹配 ARCHIVE_MANIFEST，并与当前证据文件字节一致。
4. BINARIES 中 14 份现存构建产物的大小、SHA-256 全部匹配。本地快照审查记录的身份也保持不变。

因此，历史工作区原始 SHA-256 不被误称为任意 clone 上的原始字节 SHA；跨检出的提交身份以下表 Git blob 和精确 commit/tree 为准，归档证据则保留精确捕获字节。

主任务的 [COMMIT_BINDING.json](COMMIT_BINDING.json) 已独立交叉核对，21 项 raw source hash、规范化对象和 HEAD 对象全部一致，其当前文件 SHA-256 为 `f717eed06797739a95e32581f4b9cca78346375fe305020009e2586a71c6d04b`。本报告的判断以独立核验结果为依据。

### 精确源码 Git blob 清单

| 路径 | 最终提交中的 Git blob OID |
| --- | --- |
| `app/main_window.cpp` | `02d92524afe3b2b27d01d620d53a048f7bee91ab` |
| `app/main.cpp` | `3f1fb6968214ca21e42d289d65d5aed5eb3e3e9b` |
| `infrastructure/label_discovery.cpp` | `a1b790ca29855a0e3674132e7d4dc022f44ce88b` |
| `infrastructure/label_discovery.h` | `54714973fe0eb321189865bc743bafbd460de17f` |
| `scripts/verify-package.ps1` | `792501080ef496b1aa206e7f5d2a8efd8f6ccd9e` |
| `tests/label_import_tests.cpp` | `6bb9599495e06e7b2948c913e6303ec5449f337e` |
| `tests/narrowband_analysis_capture.h` | `119225202acbff3b37e1697fe16568775bdc39a7` |
| `tests/narrowband_analysis_ui_tests.cpp` | `8867427c202be21d3fcbb760df0c6b7d9680a3d5` |
| `tests/ui_capture.cpp` | `bbb7276898d3a89286e126a5df327cc86d8e1820` |
| `tests/ui_tests.cpp` | `237dbaa3699d9531c93a95429542bd9f1c2a2e73` |
| `tests/ux_r3_axis_tests.cpp` | `5b7d99219b0e37a684f9be0529b7592f2715268d` |
| `tests/ux_r3_capture.h` | `47b01b5a6a5ce7a8ed290397ba79072099e0c4c3` |
| `tests/ux_r3_label_capture.h` | `c729dbff378b6a7a1a2e4c651b50b64aaba91094` |
| `tests/ux_r3_workflow_tests.cpp` | `3fceda3b52164240ea390250468e80259cbef08f` |
| `ui/annotations/annotation_panel.cpp` | `1189d4f958932efede63dee15dbf01bf901daca9` |
| `ui/annotations/annotation_panel.h` | `b5e1673c8cf9315ce815f3f82ad6892ce1cec9c9` |
| `ui/charts/axis_description.h` | `546266b3123b65d4fcdbb8a0c756ecdc245bdc30` |
| `ui/charts/axis_layout.h` | `6401a3121b6eecf6b5c4ba24c211c23da02cb04f` |
| `ui/charts/plot_widget.cpp` | `6e5cbf407379f551c6136d5d9bcc97798c68188d` |
| `ui/charts/plot_widget.h` | `23188388c93ae0232cca32f188bc1734dc32ad2d` |
| `ui/narrowband_workspace.cpp` | `ffd5ee2925ee59bc0394da667e013a93793e039d` |

## 功能、兼容和已闭合发现

已结合此前完整独立源码审查，复查提交中的 WAV 正向发现/反向定位、WAV/DATA 同名显式选择、大小写和路径别名冲突处理、未知 TXT 映射及预览限额。WAV 的编码、Fs、数据头尾及样本边界继续来自 RIFF 预检，未将容器当作裸 IQ。菜单和底部按钮共用导入路径；目标元数据冻结、项目/文件上下文检查、确认前不发布、取消和原子来源集合发布及重复导入行为均保留。

CompactHeader 在主辅图和实际程序入口均为默认；原 A/B/C 枚举顺序和显式 CLI 选择保留。标题栏单位/原点、短文本和完整说明，主辅图预算，以及窄带五页、十个图控件、六类图形的墨迹、热图、等比例星座和实际 plotRect 映射已审查。绘图布局变化没有改写同步、DDC 或模型结果来替代显示修复。

两项已闭合的问题在提交中仍正确保留：

- 极窄 RF 范围原先固定 12 位有效数字会使 102 MHz、±0.0005 Hz 的不同刻度显示相同。最终格式按量级和步长取至多 17 位，并稀疏重复文字；PSD/STFT 定向回归同时检查刻度唯一性和数值误差。
- 实际 `app/main.cpp` 的旧 CLI 默认 A 曾覆盖窗口默认值。最终入口默认 compact，部署脚本默认不显式传轴模式，检查主辅图实际报告；8 份最终默认启动和 6 份显式 A/B/C 报告闭合该问题。旧模式为 A 的首轮启动记录不作为默认模式证据。

测试适配仍依据实际坐标语义：旧固定边距改为真实 plotRect，未删除锚点、轴命中、游标或热图断言。DPR 鼠标事件先检查投递误差不超过 1.1 逻辑像素，再按实际事件坐标保留 1e-9 的 I/Q 逆映射阈值；色表保留唯一编辑器、稳定 owner、所属 inline/弹层及刷新父级检查。

## 测试和交付证据

本轮未改变已验证实现，故本次精确提交审核复用已亲读并重新核对绑定的测试证据，没有不必要地重跑构建或 GUI。提交中的 [VALIDATION.json](../VALIDATION.json)、[ARCHIVE_MANIFEST.json](../ARCHIVE_MANIFEST.json)、[BINARIES.json](../BINARIES.json) 与原始日志/JSON 一致。

| 范围 | 审核确认的结果 |
| --- | --- |
| Debug 相关 CTest | 单轮 14/14，928.00s |
| Release 相同 14 项 | 原 serial-r2 中 13 项通过，加 canonical geometry 补测 1 项通过；原失败日志保留，不宣称一次 14/14 |
| geometry 补测 | Qt 31/31，CTest 32.02s |
| 主 UI 原有业务 data 行 | 三组 Qt totals 31/58/23，扣除 init/cleanup 后为 106 行 |
| 窄带 UI / 轴 / 工作流 | Debug/Release 均为 Qt 17/17、17/17、27/27 |
| 标签 golden | MATLAB WAV/TXT 10 对象/65 段；TorchSig 4 标签 |
| 原生矩阵 | Debug/Release × GPU/软件，共 4 组；每组 32 宽带、26 窄带、6 标签入口，合计 256 场景/入口检查 |
| 默认独立启动 | 8/8，system-only PATH，实际主辅图均 CompactHeader |
| 显式 CLI A/B/C | Debug/Release 共 6/6 |

原生实际屏幕为本轮探测的 Redmi 27 NU，连接序号 1、Windows 设备 `\\.\DISPLAY6`，3840×2160 物理、2560×1440 逻辑、DPR 1.5。GPU 为 QRhi D3D11 / NVIDIA GeForce RTX 5060 Laptop GPU，软件结果单独标明 QPainter；offscreen 的交互测试不作为 GPU 或实屏证明。原始数值、坐标手势、布局字段、标签确认/取消和状态保全已在前轮审查逐场景核对，当前提交证据 blob 与当时记录完全一致；288 份原始 PNG 的哈希亦已核对，提交内只复制代表图并保留完整原图清单。

最后的入口修复只改变 app/main.cpp 和部署脚本，已重新构建实际 SignalStudio 并重跑全部 8+6 启动验证；未变化的共享 UI/DSP、测试及捕获程序继续绑定此前 CTest/原生矩阵。没有将旧入口运行结果混入最终默认模式证明。

## 属性、历史报告和已知边界

本目录 .gitattributes 仅对 evidence/** 及历史 INDEPENDENT_REVIEW.md 使用 -text/-whitespace，保留原始行尾，不改变源码语义或放宽代码测试。归档的 193 项提交 blob 全部精确匹配；历史本地快照审核报告在提交和工作区中均保留 SHA-256 `c8f283d992922a69bbae2b3cb3eadec769b84a244a239b0d46616571afcc18aa`。根目录更早的独立报告仍为未跟踪原文件，SHA-256 `d65fa9319a490fab0fd532e1f9401c844afbd77f7d5d4d2f35462e38bb9957ab`，未覆盖或替换。

本机最长完整路径为 273 字符，仓库相对路径最长 240 字符；Windows Git 检出须启用 core.longpaths。实际设置来源仅为当前仓库 .git/config，未发现本轮修改全局设置；DELIVERY 已说明该检出要求。source clone 不自动包含 out 中的本机二进制、运行库、模型或原有未跟踪数据，清单中的绝对运行路径属于验证机器证据。

以下边界不构成本轮规定范围内的阻塞，但不能被描述为已经通过：

- 4 个文件系统符号链接用例因本机无法创建链接而跳过。
- 旧 1600×900 整组等待状态失败仍未查明；本轮验收限定实际 4K/150%，未声称修复旧尺寸的调度问题。
- 原生标签选择器证据针对实际 Windows Qt QFileDialog，不是 Windows Shell picker。
- 本轮不新增现场 RF 精度、设备输入、模型识别、真实解调位流或 Ubuntu 已通过声明。相关 14 项 CTest 不是全部仓库测试集合。

## 远端与发布身份

本次只读 `git ls-remote origin refs/heads/rebuild/a143-native` 返回 `1608c4c87dd102fbe21e01e0d83c5e606119d50e`，与本地远端跟踪引用一致；该提交正是本次提交的父提交。祖先检查成功，左右差异为 0/1，观察时可正常快进，无需强推。审核者没有 fetch 或修改远端。

发布者应显式选用本份提交后报告执行既有发布校验，并在实际发布前再次 fetch/核对远端祖先关系和待推送身份；本次远端读数不替代发布时的竞态检查。批准只绑定本报告顶部的精确提交，不能用于其后新提交、改写历史或任意不同目标的推送。本报告和 COMMIT_BINDING 等提交后附件保持本地未跟踪，不再提交进入被批准对象。

