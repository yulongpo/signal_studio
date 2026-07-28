# Signal Studio

Signal Studio 是构建于可复用 C++20 平台之上的 Windows 离线数字信号分析软件。MS-00～MS-03 已完成平台、Core/Data/TaskRuntime、DSP/Compute、Visualization/Workbench 与真实 Qt Widgets 原型验收。当前按用户边界暂停，未确认前不进入 MS-04。

## 当前平台目标

工程导出以下稳定 CMake 目标：

`SignalStudio::Core`、`SignalStudio::Data`、`SignalStudio::DSP`、`SignalStudio::Compute`、`SignalStudio::TaskRuntime`、`SignalStudio::Visualization`、`SignalStudio::Workbench`、`SignalStudio::PluginSDK`、`SignalStudio::ModelRuntime` 和 `SignalStudio::Dataset`。

Qt 仅作为 Visualization 和 Workbench 的私有依赖。所有公共头文件均不暴露 Qt 或其他第三方类型。

## 构建与测试

Windows 本机开发基线为 MSVC 2022 x64、CMake、Ninja、Python 3，以及面向 MSVC 的 Qt 6.11.1。项目 UI 源码与安装包的最低支持版本为 Qt 6.10.3；GitHub Actions 使用同版本 `win64_msvc2022_64` 做真实最低版本门禁，不改变本机已安装实例和不可变 BL1.0 依赖选择。低于 6.10.3 时，CMake 与 UI 模块编译期守卫都会给出明确错误。仓库脚本优先复用已初始化的 MSVC 环境，否则依次通过 `VSINSTALLDIR`、`vswhere.exe` 和已知本机后备位置发现工具链：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/bootstrap.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/configure.ps1 -Preset windows-msvc-debug
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -Preset windows-msvc-debug
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Preset windows-msvc-debug
```

Release 使用 `windows-msvc-release`。CPU 预设显式关闭 CUDA；`cuda` 预设强制使用本机 CUDA Toolkit 12.4.131，缺失时明确配置失败，`AUTO` 模式才允许记录原因后降级 CPU。初始化过程不会自动安装 CUDA 或 cuDNN。

## MS-03 Qt 原型

构建 CPU Debug 后可直接启动：

```powershell
.\build\local-windows-msvc-cpu-debug\bin\signal_visualization_workbench_demo.exe
```

目标目录和安装树均包含 Qt Core/Gui/Widgets DLL、Windows/offscreen 平台插件及 `qt.conf`，无需手工配置
`QT_PLUGIN_PATH`。真实运行截图、Designer 文件和自动截图说明见
[`docs/development/ui-preview/MS-03_UI预览索引.md`](docs/development/ui-preview/MS-03_UI预览索引.md)。

若要在同一 PowerShell 进程中连续验证 Debug 和 Release，并检查 PATH 去重、环境幂等性与用户预设稳定性，执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test-same-session.ps1
```

无界面包不要求 Qt：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/configure.ps1 -Preset windows-msvc-headless-release
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -Preset windows-msvc-headless-release
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Preset windows-msvc-headless-release
```

脚本自动发现 MSVC、CMake、Ninja，并在 UI 预设中发现 Qt。生成的 `CMakeUserPresets.json` 保持忽略状态，通过一个隐藏工具链基预设只保存一份完整环境；Qt 隐藏基预设仅保存短标量根目录。PATH、`CMAKE_PREFIX_PATH`、Qt 根目录及 MSVC 路径列表均执行规范化和大小写不敏感去重。文件采用确定性内容和同目录原子替换，重复生成的字节内容必须稳定。已提交的预设与 VS Code 配置不含本机绝对路径。

VS Code 的配置、构建、测试和 F5 均使用 `local-windows-msvc-debug` 构建树。F5 前置任务会校验缓存源目录、生成器、UI 选项、目标存在性和输入新鲜度；测试目标构建后复制所需 Qt 运行库，因此不依赖 VS Code 进程继承本机 Qt PATH。

`dependencies/dependency-lock.json` 将不可变获取/包元组、可接受宿主范围和精确主机快照分开。默认 bootstrap 使用 `CompatibleHost`，接受契约范围内的补丁版本和不同安装路径；`Acquisition` 仅验证 BL1.0 获取、离线缓存和 14 个包元组；只有显式 `ExactCapturedHost` 才与 `dependencies/captured-host-evidence.json` 的版本、路径和文件哈希逐项比较。`dependencies/offline-cache-manifest.json` 记录经批准的可复现缓存，vcpkg 获取仍必须使用 BL1.0 脚本规定的固定提交 `.tar.gz` URL、大小和 SHA-256。

## 已批准基线与原型参考

不可变的 BL1.0 文档快照位于 [`docs/baseline/Signal-Studio-Dev-Docs`](docs/baseline/Signal-Studio-Dev-Docs)。原评审 Web 原型保存在快照内的 `02_原型设计/原型源文件/Signal Studio交互原型_基线归档.html`，仅用于视觉与交互参考；C++ 实现不链接、包装或依赖旧上游原型。

外部多 GB 录制数据仍位于 `../test_data`。完整性与夹具命令见 [`test_data/README.md`](test_data/README.md)。

## 项目文档

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)
- [`docs/DEVELOPMENT_PLAN.md`](docs/DEVELOPMENT_PLAN.md)
- [`docs/TEST_PLAN.md`](docs/TEST_PLAN.md)
- [`docs/DECISIONS.md`](docs/DECISIONS.md)
- [`docs/CHANGELOG.md`](docs/CHANGELOG.md)
- [`docs/api/visualization.md`](docs/api/visualization.md)
- [`docs/api/workbench.md`](docs/api/workbench.md)
- [`docs/milestones/MS-00`](docs/milestones/MS-00)
- [`docs/milestones/MS-01`](docs/milestones/MS-01)
- [`docs/milestones/MS-02`](docs/milestones/MS-02)
- [`docs/milestones/MS-03`](docs/milestones/MS-03)
