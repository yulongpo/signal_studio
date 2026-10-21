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
