# VS Code 构建与调试指南

## 1. 前置工具链

Signal Studio 在 Windows 11 / MSVC 2022 x64 / Qt 6.11.1 / CMake / Ninja 上开发。`scripts/bootstrap.ps1` 自动发现并报告本机工具链；`scripts/configure.ps1`/`build.ps1`/`test.ps1` 导入 MSVC/Qt/Ninja 环境后调用 CMake。

| 工具 | 本机路径（示例） | 备注 |
|---|---|---|
| CMake | `D:\softwares\cmake\bin` | 4.3.1 |
| Ninja | `D:\softwares\Qt\Tools\Ninja` | Qt 自带 |
| Qt | `D:\softwares\Qt\6.11.1\msvc2022_64` | 最低支持 6.10.3 |
| MSVC | VS 2022 BuildTools 14.44 | `Import-SignalStudioMsvcEnvironment` |
| CUDA | `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.4` | 可选，cuFFT GPU 后端 |

## 2. 预设

`CMakePresets.json` 定义 9 个预设；`CMakeUserPresets.json`（gitignored）由 `scripts/common.ps1` 生成本机工具链环境块。关键预设：

- `windows-msvc-headless-debug/release`：无 Qt，CPU，最快迭代。
- `windows-msvc-debug/release`：UI + CUDA（AUTO）。
- `windows-msvc-cpu-debug/release`：UI，CUDA OFF。
- `windows-msvc-cuda-debug/release`：UI，CUDA ON。

## 3. VS Code 配置（`.vscode/`）

- `settings.json`：默认 `local-windows-msvc-debug`，CMake 工具链与本机预设。
- `tasks.json`：Configure/Build/Test/Package/Clean 任务，绑定预设。
- `launch.json`：F5 调试 `signal_studio.exe`（含 `--self-test` 配置）与单测试目标。
- `c_cpp_properties.json`：IntelliSense 指向构建树 `compile_commands.json`。
- `extensions.json`：CMake Tools / C/C++ / Qt 安装提示。

## 4. 工作流

1. `scripts/bootstrap.ps1` 报告工具链。
2. `scripts/configure.ps1 -Preset windows-msvc-debug` 配置（首次）。
3. `scripts/build.ps1 -Preset windows-msvc-debug` 构建。
4. `scripts/test.ps1 -Preset windows-msvc-debug` 测试。
5. F5 启动调试 `signal_studio.exe`，或运行 `signal_studio.exe --self-test` 无头验证。

## 5. 常见问题

- **`VCToolsRedistDir` 缺失**：`scripts/common.ps1` 的 `Update-SignalStudioUserPresets` 已捕获该变量；裸 `cmake --preset local-*` 需在 shell 导出 `VCToolsRedistDir`/`UniversalCRTSdkDir`/`UCRTVersion`。
- **Qt 平台插件缺失**：offscreen 测试需 `QT_QPA_PLATFORM=offscreen` + `QT_PLUGIN_PATH=<qt>/plugins`。
- **CUDA 不可用**：`SIGNAL_STUDIO_CUDA_MODE=OFF` 或 `cpu-*` 预设，CPU 回退。
