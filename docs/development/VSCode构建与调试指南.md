# VS Code 构建与调试指南

## 1. 前置条件

本指南面向 Windows x64 本机构建。项目使用 MSVC 2022、CMake、Ninja 和 Qt 6 Widgets。
本机 Qt 位于 `D:\softwares\Qt`，但仓库中的 VS Code 配置不保存个人机器绝对路径；
首次构建前应在仓库根目录执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/bootstrap.ps1
```

该命令探测 MSVC、Ninja、Qt 和可选 CUDA，生成被 Git 忽略的
`CMakeUserPresets.json`。CUDA 不可用时使用 CPU 预设即可，应用功能不会因此失效。

## 2. F5 启动 Signal Studio

VS Code 的唯一 F5 配置名为 `Signal Studio（CPU Debug）`。它先执行
`Signal Studio: Build CPU Debug`，构建
`build/local-windows-msvc-cpu-debug/bin/SignalStudio.exe`，再带入构建树内的 Qt
插件目录启动应用。

若曾移动 Qt 或 Visual Studio，请重新运行 bootstrap，不要在 `launch.json` 中写死路径。
出现 Qt 平台插件错误时，先确认以下文件存在：

- `build/local-windows-msvc-cpu-debug/bin/qt.conf`
- `build/local-windows-msvc-cpu-debug/bin/platforms/qwindowsd.dll`
- `build/local-windows-msvc-cpu-debug/bin/Qt6Cored.dll`

## 3. 常用任务

- `Signal Studio: Build CPU Debug`：配置并构建可调试主程序。
- `Signal Studio: Test MS-04`：构建后运行带 `ms-04` 标签的应用、结果、Inspector、
  外部数据和 UI 回归。
- `Signal Studio: Configure Debug` / `Build Debug` / `Test Debug`：保留的平台全量
  Debug 工作流。
- Release、CPU 和 CUDA 的其他预设可从 CMake Tools 或终端选择；CUDA 预设要求
  已安装的 CUDA 12.4 Toolkit，不会自动安装 CUDA 或 cuDNN。

## 4. 命令行等价操作

```powershell
cmake --preset local-windows-msvc-cpu-debug
cmake --build --preset local-windows-msvc-cpu-debug --parallel
ctest --preset local-windows-msvc-cpu-debug -L ms-04 --output-on-failure
```

验证 VS Code 配置与当前构建树一致：

```powershell
python scripts/validate-vscode-workflow.py --require-configured-target
```

主程序支持以下自动化入口：

```powershell
build/local-windows-msvc-cpu-debug/bin/SignalStudio.exe --self-test
build/local-windows-msvc-cpu-debug/bin/SignalStudio.exe --startup-smoke
build/local-windows-msvc-cpu-debug/bin/SignalStudio.exe --page p02 --width 1920 --height 1080
```

`--self-test` 在 Qt 初始化前完成真实工程、导入、PSD/STFT 和结果提交闭环，适合诊断
运行时；普通 F5 不带该参数，会进入交互界面。

## 5. 调试测试

MS-04 的纯 C++ 测试程序位于：

```text
build/local-windows-msvc-cpu-debug/bin/signal_studio_ms04_tests.exe
```

可在终端按需求编号单独运行：

```powershell
build/local-windows-msvc-cpu-debug/bin/signal_studio_ms04_tests.exe --case FR-EXP-007
```

需要断点调试时，可复制现有 `cppvsdbg` 配置并把 `program` 指向该可执行文件，
`args` 设置为 `["--case", "FR-EXP-007"]`。本机专用调试配置不要提交机器绝对路径。

## 6. 安装树验证

```powershell
cmake --install build/local-windows-msvc-cpu-release --prefix build/ms04-install
build/ms04-install/bin/SignalStudio.exe --self-test --scratch build/ms04-install-self-test
build/ms04-install/bin/SignalStudio.exe --startup-smoke
```

安装消费者测试还会在独立工程中消费全部公共模块，并检查 Release VC143 运行库、
Qt DLL、`qwindows` 插件和已安装 Signal Studio 主程序。Debug nonredist 只用于本地
调试，不能进入正式安装前缀。
