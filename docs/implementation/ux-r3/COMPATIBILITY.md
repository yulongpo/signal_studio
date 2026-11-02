# UX-R3 旧测试与验收脚本兼容迁移

2026-10-23。此记录仅覆盖兼容迁移写集，不代表 UX-R3 完整回归或原生验收通过。C++ 兼容改动已交主 Agent 统一构建；本 Agent 未运行构建、CTest、原生 GUI、Git commit/push 或分支切换。

## 工程与资产合同

`MainWindow::createProject(parent, name)` 的目标是 `parent/name.ssproj`。应用层保存、打开和最近工程使用 `.ssproj`；外层校验 `magic=SignalStudioProject`、`schemaVersion=1` 和稳定 `projectId`，内层仍检查 `state.version=6`。不把 `.ssproj` 直接交给 `ProjectStore::load`，也不把旧 JSON 仅改扩展名后当作新容器。

原 `ProjectStore` v6 JSON 行为测试继续保留 JSON、原 API 与原断言，包括 `core_tests.cpp`、`annotation_store_tests.cpp`、`channel_rate_tests.cpp`、`sample_format_tests.cpp`、`detection_task_tests.cpp` 和 `detection_store_tests.cpp`。JSON 预检夹具也保留原用途。`UiTests::nativeProjectRoundtripAndInvalidImport` 另外保留显式 v6 save/load，再执行 `ProjectFileCodec::importLegacy`，比较转换前后状态 token、原 JSON 字节，并经 MainWindow 打开转换结果继续执行原视图、标记、多文件及错误回退断言。

检测 run/chunk 的根目录改读 `session().project().assetDirectory`。新建默认是 `<file>.ssproj.assets/<projectId>`；不能继续由工程文件的父目录推导。任务冻结、取消前缀、chunk hash/字节和重开后的来源断言保留。

## 文件清单

| 文件 | 兼容变更 |
|---|---|
| `tests/ui_tests.cpp` | 新建、保存、重开及最近工程改用 `.ssproj`；增加 v6 显式转换与原文件保全断言；自动标签后手动导入的幂等断言；最近工程 Discard；旧窄带用例覆盖五页并从眼图页编辑眼图参数 |
| `tests/ui_capture.cpp` | 新建快捷键依次处理 Discard、Yes 和新建表单，实际创建 `.ssproj` 后保留空工程场景断言 |
| `tests/annotation_editing_tests.h` | 应用层目标改 `.ssproj`；原 JSON 预检保留 |
| `tests/round_two_ux_tests.h` | 应用层往返目标改 `.ssproj` |
| `tests/task_ux_tests.h` | 从 assets 读取取消任务与不可变 chunk，原取消、未读覆盖和前缀断言保留 |
| `tests/annotation_editing_capture.h` | 原生编辑后保存/重开改 `.ssproj`，原证据断言保留 |
| `tests/annotation_channels_capture.h` | 当前工程保存改 `.ssproj`，原 JSON 预检和历史 v5 场景未删除 |
| `tests/spectral_loading_capture.h` | Partial 已读前缀工程改 `.ssproj` |
| `tests/ui_refinement_capture.h` | 原生工程目标改 `.ssproj` |
| `tests/wideband_detection_capture.h` | 创建目标、chunk 根目录、自动 SigMF 标签后重导入校验 |
| `tests/wideband_adaptation_capture.h` | 取消任务持久化从 assets 读取 |
| `tests/wideband_ux_capture.h` | 工程路径、检测资产根目录、容器/内部 v6 双层断言，菜单重开支持 Discard |
| `scripts/verify-ux-unavailable.ps1` | 接受 `.ssproj` 或恰好含一个 `.ssproj` 的目录；旧 JSON 要求先显式转换；只复制当前工程及伴随资产，引用模式资产也转为隔离副本 |
| `scripts/verify-package.ps1` | 五个不同页索引检查；AnalysisProject 要求 `.ssproj`；原 GPU 与独立部署条件保留 |
| `scripts/gpu_smoke.ps1` | 五个不同页索引检查；原逐页 GPU draw-call 条件保留 |
| `scripts/screenshot.ps1` | NarrowbandAnalysis 更新为完整 18 场景，分别校验独立星座/眼图顶点数据与软件回退 |

未修改 `scripts/build.ps1`、新 UX-R3 脚本、`tests/ux_r3_capture.h`，以及主 Agent/G5 所负责的 `tests/project_file_tests.cpp`、`tests/label_import_tests.cpp`、`tests/ux_r3_axis_tests.cpp`、`tests/narrowband_analysis_ui_tests.cpp`、`tests/narrowband_analysis_capture.h`。未改 480 秒超时，未删测试或以放宽业务断言消除失败。

## 标签、页面与原生证据

可信 SigMF 自动发现由异步回调发布。`torchSigLabelsNativeRoundtrip` 先等待自动导入 4 条、1 个 sourceSet，再连续两次手动导入，核对数量、完整 sourceSet、全部 annotation 编码；保存重开并等待 SourceLoader 完成后继续检查数量、IDs 和原始证据。宽带原生检测 capture 也在手动重导入前保存自动导入快照，要求重导入不改变来源数或完整 annotation。原生成器 TXT 重复导入断言继续保留。

五页包验证要求索引恰好覆盖 `0,1,2,3,4`，对应主程序 `0x1f` 完成掩码；不会仅凭数组长度接受重复页面。每页 GPU draw-call 断言保留。识别模型不可用不妨碍校验识别页的真实可见时频图，不把模型不可用当成跳过 GPU 证据的理由。

G5 的 `narrowband-analysis-ux-r3` 报告使用 ci16/cf32 各 9 场景：DDC、原始星座、手工 Rs 眼图、恢复星座、恢复眼图、识别不可用、解调不可用、重开眼图和取消。脚本核对这 18 个唯一场景名称，拒绝旧 12 场景或重复/缺失场景。GPU 路径仍要求实际 D3D11 和 draw-call，允许热图纹理或图形顶点上传；独立星座/眼图场景额外要求顶点上传。软件路径要求 QPainter 且 GPU draw/texture/vertex 计数为零。屏幕身份、原生像素、150% DPI、全屏与逻辑尺寸检查保留；指定实屏还检查 Redmi 27 NU 及首选 4K，辅助显示不冒充实屏通过。

## 已完成的兼容自检

- 四份修改后的 PowerShell 脚本均通过 `System.Management.Automation.Language.Parser::ParseFile`。
- 两份五页脚本共 6 项合成报告检查通过：完整五页接受，四页和重复索引拒绝。
- `screenshot.ps1` 的 7 项合成报告检查通过：接受独立顶点图，拒绝眼图仅纹理证据、旧场景数、重复场景、替代物理屏及软件报告的 GPU 上传。
- 不可用场景副本脚本的 5 项夹具检查通过：相对/绝对资产复制隔离、原工程字节不变、共享父目录无关文件不复制、唯一工程目录接受、多工程目录和旧 JSON 拒绝。
- `git diff --check` 通过。

脚本逻辑证据位于 `out/tasks/wideband-detection/UX-R3/diagnostics/compatibility-capture-gates.json` 和 `compatibility-copy-probe.json`。合成报告仅验证脚本条件及资产副本逻辑；未运行 Qt、DSP、模型或实际显示器验收，不是截图/渲染证据。

## 集成结果与保留边界

1. 主 Agent 已统一构建并执行受影响 UI 与 capture，Release 完整 40/40，主 UI 106 和窄带 11 业务行无漏项；Debug 全量的历史超时及最终轮次见 VALIDATION。新建、最近工程、JSON 转换、Partial、标签重导入、检测取消前缀均有对应回归。各 UI 分组 `TIMEOUT=480` 保持。
2. 五页 CLI 以明确记录的 1 MHz 手工 Rs 假设准备内置样例，保留逐页 GPU data draw 检查；GPU/软件独立部署六组合通过。G5 指定实屏 Debug/Release × GPU/软件各 18/18。实际调用入口和原始 JSON 见 VALIDATION；不把合成报告检查计作原生通过。
3. 历史 U3 capture 的 `interactionChannelScenes` 仍要求加载冻结 v5、生成 `.pre-v6-v5-*.bak*` 并重存，与当前 ProjectStore 仅接受 v6 的规则冲突。保留了原 fixture、调用和断言，不能宣称该历史场景已通过。需要主 Agent 单独决定历史场景的归档/替代验收范围；本迁移不恢复 v5 支持，也不删除或弱化旧断言。
4. 原生不可用检查调用者必须先将旧 v6 JSON 显式转换为 `.ssproj`。脚本只复制工程容器和资产，真实 IQ 仍由原绝对源引用/重定位合同读取；需在真实样本环境确认来源可用。

资产副本检查的临时小夹具仍在报告的 `fixtureRoot` 下：自动审批审核拒绝了清理操作，仅返回 `blocked by policy`，未提供更细原因；原工程和其它工作区文件未受影响。
