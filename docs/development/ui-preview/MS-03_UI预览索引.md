# MS-03 Qt 界面预览索引

## 1. 预览范围

本目录保存 MS-03 的真实 Qt 6 Widgets 运行截图。截图来自
`signal_visualization_workbench_demo.exe`，不是 HTML 原型、设计稿或静态界面替代品。演示程序显式注入演示帧、检查器条目、任务和结果；生产
`SignalStudioWorkbench` 默认不伪造数据，未绑定内容时显示真实空状态。

| 逻辑窗口 | 截图 | 物理像素 | 用途 |
|---|---|---:|---|
| 1280×720 | [MS-03_工作台_1280x720.png](MS-03_工作台_1280x720.png) | 1920×1080 | 最小批准布局、Dock 和三图检查 |
| 1600×900 | [MS-03_工作台_1600x900.png](MS-03_工作台_1600x900.png) | 2400×1350 | 常用桌面布局检查 |
| 1920×1080 | [MS-03_工作台_1920x1080.png](MS-03_工作台_1920x1080.png) | 2880×1620 | 全高清逻辑布局检查 |
| 1920×1065，缩放因子 2 | [MS-03_工作台_200百分比DPI.png](MS-03_工作台_200百分比DPI.png) | 3840×2130 | 200% DPI、控件命中区和文字裁剪检查 |

前三张截图在本机 Windows 150% 显示缩放下生成，因此物理像素为逻辑尺寸的 1.5 倍。高 DPI 截图通过
`QT_SCALE_FACTOR=2` 独立生成；表中的逻辑尺寸和物理尺寸均由实际图像核验，不把物理像素误写为布局尺寸。

截图 SHA-256：

| 文件 | SHA-256 |
|---|---|
| `MS-03_工作台_1280x720.png` | `038e4d7b9577c505416deed8da4b41a20ad71f311cd200b672fe68c525c7adbc` |
| `MS-03_工作台_1600x900.png` | `ce09a5f7929d918a77fafdff750831d42f3508a89c30637e1d80e482cdafac3f` |
| `MS-03_工作台_1920x1080.png` | `85b7fc32db88ed1a526b802770b2d2882c93bfcd506767d6181592dfd6253ecf` |
| `MS-03_工作台_200百分比DPI.png` | `6c6c256960af2a69a85dd83266259dd657b6b2e991df92e86259247fc3720e7b` |

## 2. Qt Designer 源文件

以下 `.ui` 文件均为生产构建输入，由 CMake 的 `qt_wrap_ui` 调用 `uic` 生成头文件：

- `src/platform/workbench/ui/SignalWorkbenchMainWindow.ui`
- `src/platform/workbench/ui/SignalInspectorPanel.ui`
- `src/platform/workbench/ui/SignalTaskCenterPanel.ui`
- `src/platform/workbench/ui/SignalResultCenterPanel.ui`
- `src/platform/workbench/ui/SignalSettingsPanel.ui`
- `src/platform/workbench/ui/SignalDiagnosticsPanel.ui`

可在 PowerShell 中打开主窗口：

```powershell
& 'D:\softwares\Qt\6.11.1\msvc2022_64\bin\designer.exe' `
  '.\src\platform\workbench\ui\SignalWorkbenchMainWindow.ui'
```

各文件只使用布局管理器组织控件，不以大量绝对坐标构造页面。图谱 Canvas、视口联动和业务命令由 C++ 运行时装配，保持 Designer 布局与业务状态解耦。

## 3. 编译与启动

```powershell
.\scripts\configure.ps1 -Preset windows-msvc-cpu-debug
.\scripts\build.ps1 -Preset windows-msvc-cpu-debug
.\build\local-windows-msvc-cpu-debug\bin\signal_visualization_workbench_demo.exe
```

构建目标会把 `Qt6Core[d].dll`、`Qt6Gui[d].dll`、`Qt6Widgets[d].dll`、Windows/offscreen 平台插件和
`qt.conf` 部署到可执行文件旁。即使清空 `QT_QPA_PLATFORM`、`QT_PLUGIN_PATH` 与
`QT_QPA_PLATFORM_PLUGIN_PATH`，程序仍可通过默认 Windows 平台启动，不依赖开发者手工设置插件路径。

## 4. 自动截图

```powershell
.\scripts\capture-ms03-ui.ps1 `
  -BuildDirectory .\build\local-windows-msvc-cpu-debug `
  -OutputDirectory .\docs\development\ui-preview
```

脚本依次生成 1280×720、1600×900、1920×1080 和 200% DPI 四组截图，并对进程退出码、输出文件存在性和非空文件进行检查。截图仅用于可视验收；功能结论来自具名 CTest、安装消费者和默认 Windows 平台启动回归。
