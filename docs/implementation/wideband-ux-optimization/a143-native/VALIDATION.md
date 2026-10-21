# 本轮验证记录

## W0 稳定基线（2026-10-21）

产品源码为 `4904740`，C++20 / Qt 6.11.1 / Visual Studio 18 2026。两个配置均重新构建，不能与之前 grok 的输出程序混用。W0 只有新增测试目标及文档，没有修改产品源码。二进制 SHA-256 记录在 `out/tasks/wideband-ux-optimization/a143-native/W0/diagnostics/baseline-identity.json`。

| 验证 | 实测 | 命令 / 证据 |
|---|---|---|
| Debug 全部 CTest | 23/23，443.89 秒；UI 294.64 秒 | `scripts/build.ps1 -Configuration Debug -Jobs 8`；W0/logs/debug-build-test.txt |
| Debug Qt UI | 108/108，0 失败、0 跳过 | W0/diagnostics/debug-ui-baseline.txt、.xml、.timing.jsonl |
| Release 构建 | 成功 | `scripts/build.ps1 -Configuration Release -Action Build -Jobs 8`；W0/logs/release-build.txt |
| Release 非 UI | 22/22，36.32 秒 | `ctest --test-dir out/vs2026-qt611-release -C Release -E signal_studio_ui$ --output-on-failure`；W0/logs/release-ctest-non-ui.txt |
| Release UI CTest | 1/1，59.47 秒；与非 UI 合计覆盖全部 23 项 | `ctest --test-dir out/vs2026-qt611-release -C Release -R signal_studio_ui$ --output-on-failure`；W0/logs/release-ctest-ui.txt |
| Release Qt UI | 108/108，0 失败、0 跳过 | W0/diagnostics/release-ui-baseline.txt、.xml、.timing.jsonl |
| Debug 原生 GPU 演示 | 通过，D3D11、硬件驱动、至少一次热图上传 | `scripts/screenshot.ps1 -Configuration Debug`；W0/assets/baseline-wideband/ |
| Release 基础检测原生旅程 | 23/23 | `scripts/screenshot.ps1 -Configuration Release -WidebandDetection`；W0/assets/baseline-interactions/wideband-detection-report.json |
| 新增风险复现（修复前） | 两个业务用例均失败，符合缺陷预期 | `SignalStudioUxTests.exe`；W0/diagnostics/reproduction-before-fix.txt |

表内 `W0/` 均相对 `out/tasks/wideband-ux-optimization/a143-native/`。两个 GUI 配置分开运行，未并发使用同一 UI 设置。离屏测试不证明 GPU 渲染。

原生屏幕本次实探：Redmi 27 NU、连接序号 2、`\\.\DISPLAY6`、3840×2160、DPR 1.5、2560×1440 逻辑全屏。选择依据是当次唯一符合实际尺寸及 DPI 的连接屏，非设备路径后缀。未执行任何 1366×768 任务。

基础检测旅程明确启用 `pipeline-fixture-v1`，不执行正式模型。Debug 原生演示为演示数据；两者均不证明真实 RF 精度、正式 TensorRT 前向或本轮优化效果。

### 修复前缺陷证据

- `uxD01_historicalDerivation`：点击真实“派生新运行”后 `parentRunId` 没有从历史请求带入，后续快照断言尚未执行。
- `uxD03_crossFileCancellation`：文件 B 的卡片取消按钮仍为启用状态，目标实际为文件 A 的未结束任务。
- 日志 Qt 总计 `2 passed, 2 failed`；其中两个通过是初始化、清理，实际业务为 `0/2`。没有设置 WILL_FAIL 或把失败伪装成正常回归通过。W1 修复后才注册正常 CTest。

## W1 检测操作（2026-10-21）

局部移植bed43b2：按值复制历史请求、核对文件、parentRunId及custom冻结范围；普通来源查看不configure历史。右侧取消只作用当前文件聚焦的未结束runId，终态禁用；其他文件任务提供定位。列表查看保留同文件输入草稿。顶部/图谱“检测当前区域”明确选择visible，普通检测入口保持已有范围。

- Debug构建成功；独立UX CTest通过，5个业务用例、Qt7/7，0失败/跳过。最终版本覆盖跨文件派生、同文件未提交草稿、跨文件取消及终态、当前视区入口、真实ROI拖动确认/取消。日志W1/logs/ux-extended-ctest.txt和构建树ux-test-results-debug.txt。
- 检测存储、协议、任务、core相关CTest 4/4，34.43秒，W1/logs/related-ctest.txt。
- 已有widebandRoiPreservesManualTool、widebandFixtureTaskNativeRoundtrip通过。widebandConfigurationCommittedDrafts初测失败：旧测试把“检测当前区域”当成仅打开，期待保留custom范围。按获批入口分工，将原强断言绑定普通“宽带检测”入口，保留原requestedRange相等断言，另有独立用例证明当前区域确实选择visible。该用例重测通过（Qt3/3），旧失败和修正后日志均保留在W1/diagnostics。
- 本节点不声称Release完整回归或新布局通过；W2–W6尚待实施。

## W2 图谱工具栏与设置层（2026-10-21）

新增窗口直接宿主SettingsPopoverHost、单行ChartControlStrip，PSD常用项/波形分量与主图STFT N/唯一colormap按所属图谱分布。电平和显示选项各有独立设置层；原谱高级编辑器只在创建时迁移。驻留帧PSD显示STFT N，保留统计PSD的独立N。窄带提供对应设置入口。

- 当前Debug独立UX通过：10业务用例，Qt12/12，CTest29.93秒。`W2/logs/ux-routed-ctest.txt`，原始日志/身份在`W2/diagnostics/ux-final.{txt,xml}`及`identity.json`。覆盖1920×1080与2560×1440离屏单行布局、命中、溢出、内部下拉、原生窗口路由、自动收起、Esc、失焦/重开、上下文、焦点/父对象及草稿保持。
- 迁移后9个既有业务用例全部通过，Qt11/11、48.346秒，`W2/diagnostics/existing-corrected.txt`。保留滚轮不改值与真实滚动容器、方法与缓存独立、全局显示设置及布局持久化断言。首次组合运行在新增跨工程测试的确认框中等待，已终止并保留旧日志；补齐可见确认框点击后全部通过。
- Debug本轮独立原生旅程28/28通过，命令`scripts/screenshot.ps1 -Configuration Debug -WidebandUx`。报告`W2/assets/native-final/wideband-ux-report.json`，图片在报告同目录唯一scenes子目录。实际屏幕Redmi27NU/连接2/DISPLAY6，3840×2160、150%、逻辑2560×1440全屏，D3D11硬件与真实IQ热图上传。检查了截图、控件中心命中、并列控件边界、真实鼠标下拉选择、Enter提交、电平适配、显示、弹层关闭、最大化恢复及侧栏收起后的图谱操作。
- 新增原生捕获入口与脚本校验独立于历史R/U计数；该模式本阶段不运行正式模型。当前全屏无须溢出时如实记录；溢出点击与定位由离屏组件用例覆盖。窗口缩放重定位在离屏测试，原生全屏保持实际DPI。
- 原生迭代失败报告均保留：发现并修复了设置层误判原生QWindow内部点击、长PSD表单遮挡下一图谱标题栏、Esc被工作区优先处理，以及旋钮隐藏/显示误提交和重置草稿。自动化下拉遵守Qt双击保护间隔；最大化时只检查当前可操作图谱，不将背景覆盖层当成可操作目标。
- W6仍需在最终源码完成Debug/Release全量回归、四页上下文及正式模型旅程。本阶段不外推正式推理、GPU DSP或真实RF准确度。

## W3 谱方法与合法配置（2026-10-21）

- Debug构建通过；新增UX累计15个业务用例、Qt17/17，CTest34.62秒，`W3/logs/ux-final-ctest.txt`。新增覆盖Welch/Bartlett/DPSS/Burg首次默认与返回恢复、Kaiser切换、未提交/非法值不进历史/自定义行/QSettings、完整表单行显隐、PSD/STFT独立、驻留帧N与统计N隔离、非法草稿Tab/失焦/外部点击/重开/Esc恢复。
- 谱数值CTest通过，1/1、1.12秒，`W3/logs/ux-spectral-ctest.txt`。5个既有参数/缓存/驻留帧业务用例最终通过，Qt7/7、45.115秒，`W3/diagnostics/related-final.txt`。首测两个失败保留：旧测试期待非法文本恢复为0，已按获批规则改为验证草稿/错误保留；Bartlett无关重叠字段整行隐藏，同时保持禁用状态，保留既有约束。
- 合法方法历史仅在SpectralSettingsWidget实例内，不扩展v6、QSettings工程配置或计算协议。进入Bartlett使用0重叠，不写入Welch历史。源工程中精度高于旋钮显示的合法值按原值缓存，防止显示舍入变成方法切换编辑；直接用例覆盖该边界。
- 临时层中的旋钮、可编辑下拉与AdaptiveValueEdit非法提交都保持已提交业务值，同时保留草稿、字段错误及可恢复文本。显示纯刷新不缓存草稿；方法切换撤销该方法未提交文本。有效自定义值才更新最近值。
- 本阶段没有正式模型前向声明；最终布局原生场景、全量两配置回归仍在W6执行。

## W4 右侧上下文与检测首屏（2026-10-21）

- Debug 产品、UX、UI、原生捕获目标构建通过。独立 UX 累计 21 个业务用例、Qt 23/23，CTest 60.14 秒，`W4/logs/ux-final-ctest.txt`。相关既有 11 个业务用例、Qt 13/13、37.854 秒，`W4/diagnostics/related-final.txt`；覆盖布局/最近页、色表宽窄带往返、配置草稿、ROI、直接标记编辑/删除/来源快照和文件菜单。
- 新用例直接验证四页显式入口与当前上下文手动覆盖、换文件解除覆盖、后台刷新不切页；文件有效带宽仍按文件保存；唯一色表仅工作区转换时切换宿主；模型×谱完整编码请求等价；所有折叠参数与非法草稿；1920×1080及2560×1440离屏首屏基础配置、预检和运行区可达。失效语义的隐藏草稿在显式策略切换时撤销，单纯收起分组不撤销。
- 真实 4K/150% 原生最终旅程 39/39，`scripts/screenshot.ps1 -Configuration Debug -WidebandUx`，报告 `W4/assets/native-final/wideband-ux-report.json`。屏幕 Redmi 27 NU / 连接2 / DISPLAY6、3840×2160、DPR1.5、逻辑2560×1440全屏；三图D3D11硬件，实际IQ热图上传。新增基础检测首屏、五种详情折叠、显示摘要、文件属性、空标记引导、真实右键ROI与Esc恢复。已查看最终首屏与显示页截图；电平摘要舍入显示，编辑值与请求精度不变。
- 初测失败及诊断保留：演示数据没有有效检测命令不能用作“当前区域”入口夹具，改用实际IQ；重复点击已选页会产生真实焦点迁移，辅助函数只在需要时切页；最近布局测试等待实际源读取就绪后验证标记选择。离屏插件将已隐藏QMenu保留为活动窗口，测试改从可见ROI标题按钮验证工具/页签契约；真实Windows右键QMenu路径由上述原生旅程独立验证。没有增加隐藏旧控件或 trigger 兜底。
- 本节点不证明正式模型前向、最终两配置全量回归或全部新任务操作；这些仍在W5/W6。

## W5 任务操作与键盘（2026-10-21）

- Debug 产品、UX/UI目标构建通过。累计24个UX业务用例、Qt26/26，CTest37.23秒，`W5/logs/ux-final-ctest.txt`、`W5/diagnostics/ux-final.txt`。既有真实文件夹具任务重开、配置草稿两用例通过，Qt4/4、9.211秒，`W5/diagnostics/related-final.txt`。
- Return查看来源、Alt+Return详情、Ctrl+Return派生、Ctrl+Shift+X取消均直接发送键盘事件，并断言焦点、QAction发出次数、目标runId、历史参数、草稿与实际任务状态；没有额外trigger兜底。详情中完整冻结请求、覆盖说明和折叠JSON与原始记录逐项一致。同文件查看及详情保留草稿，派生明确复制快照且不运行。
- 新建实际IQ与显式pipeline-fixture任务，待已完成窗口和不可变分片落盘后修改图谱PSD/STFT N、色表、电平，再切到B。B的模糊取消禁用；列表选择B终态后按取消不影响A，明确选A后键盘取消为Cancelled，B仍Completed。核对冻结请求完整编码、首分片字节与SHA、已读/未读覆盖、取消前缀及重新加载的持久化记录。该用例验证真实系统链路，不代表正式模型推理。
- 行内四操作真实点击及可访问文本通过；按下后删除原行再刷新，释放不会派生替代行。任务卡删去长范围/覆盖段落，保留真实计数与首条原因，完整技术证据仍可打开。Return统一由QAction处理，避免QListView activated重复执行。
- 首轮失败保留在W5/diagnostics：测试误对不可编辑N下拉取lineEdit导致崩溃；修正为真实键盘选项选择。关闭模态详情后先恢复主窗口激活/列表焦点，再送快捷键；首轮立即送键没有激活派生。行文本断言按实际“完成”校正，保留状态、模型、阶段、结果四项语义检查。

## W6 最终结果（2026-10-21）

本节替代 W6 迭代期间的计数和耗时；W0–W5 表格仍表示各阶段当时结果。最终产品、测试及脚本源码提交为 `399b85e970f2f22432cd95277dc30e9075ca4cb4`。二进制在提交前由该工作区内容完成构建与验证，构建时嵌入的 Git 基线为 `afefd73`；不能把该版本字符串误作最终源码身份。[source-identity.json](evidence/source-identity.json) 记录 249 个构建/源码/资源/测试/脚本文件的 Git blob 和 SHA-256、10 个 Debug/Release 程序的 SHA-256、Qt 6.11.1 及 VS18 2026 x64。源文件清单摘要为 `b2dc55e9895061db20b8c4e3df7ad4a27ca4d083e652e1dab0f81b5ce637bbc7`。此后提交只归档文档和证据。

表内 W6 路径相对 `out/tasks/wideband-ux-optimization/a143-native/W6/`。归档 JSON/TXT 是最终原始文件副本，不覆盖旧失败报告。

| 验证 | 最终实测 | 原始日志 / 本目录归档 |
|---|---|---|
| Debug 构建及全量 CTest | 28/28；601.04 秒；UI CTest 416.02 秒 | `logs/build-debug-final-ui.log`、`logs/debug-ctest-published.txt`；[debug-ctest.txt](evidence/debug-ctest.txt) |
| Release 构建及全量 CTest | 28/28；114.75 秒；UI CTest 71.58 秒 | `logs/build-release-final-ui.log`、`logs/release-ctest-published.txt`；[release-ctest.txt](evidence/release-ctest.txt) |
| 基础 Qt UI | 两配置各 108 PASS，0 FAIL/SKIP；106 业务用例；原始 Qt 耗时 Debug 415.238 / Release 71.299 秒 | [debug-ui.txt](evidence/debug-ui.txt)、[release-ui.txt](evidence/release-ui.txt) 及对应逐项 timing.jsonl |
| 独立 UX 五组 | 两配置各 26 业务用例；Qt 合计各 36 PASS，0 FAIL/SKIP | `evidence/ux-*-test-results-{debug,release}.txt` 及 JUnit XML；五组 CTest Debug 57.05 / Release 11.35 秒 |
| Debug 原生 UX | 57/57，正式两模型、键盘取消、CPU DDC、v6 重开全部通过 | `assets/ux-debug-published/wideband-ux-report.json`；[native-debug.json](evidence/native-debug.json) |
| Release 原生 UX | 57/57，同上 | `assets/ux-release-published/wideband-ux-report.json`；[native-release.json](evidence/native-release.json) |
| Release 正式来源兼容旅程 | 43/43，真实 FFSCN Sample/ROI、三来源标记、编辑/批选/导出/拆分合并/关系/通道与重开 | `assets/production-release-published/wideband-detection-report.json`；[production-release.json](evidence/production-release.json) |
| 两配置宽带 GPU 独立启动 | 2/2，硬件 QRhi、实际热图上传、4K/150% | `assets/gpu-{debug,release}-published/`；`evidence/gpu-{debug,release}.json` |
| 两配置窄带独立部署 | 2/2，四页、硬件 QRhi、嵌入 IQ 与 CPU DDC | `assets/package-{debug,release}-narrowband-published.json`；同名去 published 的 evidence JSON |
| 两配置软件回退独立启动 | 2/2，明确 QWidget，hardwareRenderer=false | `assets/package-{debug,release}-software-published.json`；同名去 published 的 evidence JSON |
| Release 缺模型 / 受控无兼容设备 | 2/2，原因与详情可见，基本显示、标签、手动标记和历史仍可使用 | `local-builds/dependency-refusal-published/`；[missing-models.json](evidence/missing-models.json)、[no-compatible-gpu.json](evidence/no-compatible-gpu.json) |

独立 UX 分组业务数为检测 9、谱设置 5、图谱 2、布局 7、设置层 3；每组另有初始化/清理，因此 Qt 数为 11、7、4、9、5。原生场景数与业务用例数分别计数。UI 480 秒终止保护保持不变，CTest 包装同时检查 Qt 原始 FAIL!/XPASS/QFATAL 和 Totals，退出码为零不能掩盖断言失败。

### 原生屏幕、模型及数据身份

最终报告均按执行时 Windows DisplayConfig 选择唯一实际 4K/150% 外接屏：Redmi 27 NU，连接序号 2，原生 GDI 路径 DISPLAY6，3840×2160、DPR 1.5、逻辑 2560×1440，全屏。完整 monitorDevicePath 保存在 JSON。没有执行 1366×768 工作；1920×1080 只用于离屏布局。

三个显示图谱为 QRhi D3D11 / AMD Radeon 880M，报告包含硬件帧与主图实际纹理上传。正式模型由独立 TensorRT 10.11.0.33 worker 在 NVIDIA GeForce RTX 5060 Laptop GPU 前向：两配置均 FFSCN 3/3、SCN 2/2 窗口 Completed。数据为用户已有 TorchSig SigMF CF32 磁盘文件：400000 样本、20 Msps、中心 2.5 GHz；IQ SHA-256 `3a47e55b5f02851d702d5c5782998bf3e374f67cde592c35689acf9ec5399254`，元数据 SHA-256 `5efd03dae0f53002c8400ddc2ba65e367d7dde315b232a24cd77a2b1bd982a29`。

正式长任务中通过可见图谱控件修改 N、色表、动态范围，随后向选中的任务行真正发送 Ctrl+Shift+X。Debug 保存 24/1998、Release 保存 30/1998 个完成窗口，状态 Cancelled；冻结请求、已有分片及前缀 SHA 保持。手动编辑标记经可见通道入口得到各 8192 个真实 CPU DDC 样本，输出 SHA 经文件菜单保存、完整路径输入 Qt JSON 选择器并重开后再次一致；v6ReopenVerified=true。

缺模型检查使用完整部署包的独立复制件，仅复制时排除模型目录。无兼容设备检查在独立复制进程及 worker 中设置 CUDA_VISIBLE_DEVICES=-1；未改名或移除原部署资产。资产/配置预检与实际设备可用性分别处理：前者可能仍合法，worker 运行后明确报告 `Required CUDA 12.9 runtime/driver/device is unavailable`，任务失败且完成窗口为零。两场景均不声称模型推理成功。独立启动仅使用部署目录和系统 PATH，不依赖外部源码或 Qt 安装目录。

### 命令与复跑入口

构建对应公共 preset，最终分别运行以下 CTest；构建输出见上表，构建入口也可使用 `scripts/build.ps1 -Configuration Debug/Release -Action Build -Jobs 8`。原生命令分别指定 Debug 与 Release；这里列出 Release 的最终输出目录，复跑应选择新的目录以保留已归档结果。

```powershell
ctest --test-dir out/vs2026-qt611-debug -C Debug --output-on-failure
ctest --test-dir out/vs2026-qt611-release -C Release --output-on-failure
scripts/screenshot.ps1 -Configuration Release -WidebandUx -OutputDirectory out/tasks/wideband-ux-optimization/a143-native/W6/assets/ux-release-published
scripts/screenshot.ps1 -Configuration Release -WidebandDetection -ProductionModel -OutputDirectory out/tasks/wideband-ux-optimization/a143-native/W6/assets/production-release-published
scripts/gpu_smoke.ps1 -Configuration Release -OutputDirectory out/tasks/wideband-ux-optimization/a143-native/W6/assets/gpu-release-published
scripts/verify-package.ps1 -Configuration Release -Narrowband -ReportPath out/tasks/wideband-ux-optimization/a143-native/W6/assets/package-release-narrowband-published.json
scripts/verify-package.ps1 -Configuration Release -SoftwareRenderer -ReportPath out/tasks/wideband-ux-optimization/a143-native/W6/assets/package-release-software-published.json
```

受控依赖检查入口为 `scripts/verify-ux-unavailable.ps1 -Configuration Release -Project <本轮生产旅程工程的project.json> -OutputDirectory <新的W6/local-builds子目录>`。最终使用 `production-release-published/wideband-project-faedf6cc-18c4-402a-abdf-e7c125d6761b/project.json`，结果保存在上表 dependency-refusal-published 目录。该脚本拒绝已有输出目录，并恢复进程环境。

### 失败迭代与修复归属

- 首次全量两配置各有旧 UI 断言失败：旧文件属性区域已不形成原滚动容器，频率坐标控件已进入显示层。测试迁移到实际方法表单滚动区和显示层，保留滚轮不改值、容器实际滚动、多段选择等断言；没有删除业务用例降低门槛。
- 审查发现右侧设置跳转在同入口再次点击时没有关闭，以及顶部跨文件派生把旧草稿提前提交。修正物理锚点判断和派生命令草稿策略，直接用例通过。
- 原生长表单字段仅中心可见不足以证明完整可操作，捕获工具改为确保整个控件矩形可见，再检查边界、命中和实际输入。文件选择器的 programmatic selectFile 没有选择成功，旧中断保留；最终改用可见完整路径输入与 Enter，完整 Save/Reopen 通过。
- 任务筛选器先显示空列表、后加入长 runId 时出现裁切。原菜单代理会重复计算边距，首次宽度补丁仍失败；最终使用标准列表代理、实际行/字体宽度与屏幕约束，后台刷新保留模型及键盘当前项。离屏使用可容纳的代表 ID 验证尺寸和刷新，真实全 UUID 由最终 4K 原生兼容旅程点击验证。
- 正式任务冻结性比较改为完整按值快照而非共享指针地址；旧 fixture 的唯一谱型本来禁用，捕获明确断言其受限默认值，不向禁用控件发送伪操作。全部最终用例无隐藏旧控件或额外 trigger 兜底。

旧失败/超时/中断日志和诊断都留在 W6/logs、diagnostics、assets、local-builds，不把这些中间结果宣称为通过。最终通过只对应上表 published 结果。

本轮证明 UI/交互兼容、实际模型前向、显示硬件、CPU DDC 和保存重开；未执行批量 G/T 效果矩阵、真实 RF 精度评估、Ubuntu 运行或新的全配置压力/性能验收。窄带星座、识别和解调示例保持合成标识。
