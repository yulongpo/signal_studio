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
