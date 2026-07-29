# MS-03 Qt 界面预览索引

## 1. 预览范围

本目录保存 MS-03 原型对齐后的真实 Qt 6 Widgets 运行截图。截图来自 `signal_visualization_workbench_demo.exe`，不是 HTML、QWebEngine 或静态界面替代品。演示程序显式注入演示帧、任务和结果；生产 Workbench 默认不伪造数据。

| 页面 | 逻辑窗口 | 请求缩放 | 物理像素 | 截图 |
|---|---:|---:|---:|---|
| P02 | 1280×720 | 100% | 1280×720 | [最小布局](MS-03_P02_1280x720_100百分比.png) |
| P02 | 1600×900 | 100% | 1600×900 | [标准对比尺寸](MS-03_P02_1600x900_100百分比.png) |
| P02 | 1920×1080 | 100% | 1920×1080 | [1080P](MS-03_P02_1920x1080_100百分比.png) |
| P02 | 3840×2160 | 100% | 3840×2160 | [4K](MS-03_P02_3840x2160_100百分比.png) |
| P02 | 1920×1080 | 125% | 2400×1350 | [125% DPI](MS-03_P02_1920x1080_125百分比.png) |
| P02 | 1920×1080 | 150% | 2880×1620 | [150% DPI](MS-03_P02_1920x1080_150百分比.png) |
| P02 | 1920×1080 | 175% | 3360×1890 | [175% DPI](MS-03_P02_1920x1080_175百分比.png) |
| P02 | 1920×1080 | 200% | 3840×2160 | [200% DPI](MS-03_P02_1920x1080_200百分比.png) |
| P04 | 1600×900 | 100% | 1600×900 | [任务中心](MS-03_P04_1600x900_100百分比.png) |
| P07 | 1600×900 | 100% | 1600×900 | [设置与诊断](MS-03_P07_1600x900_100百分比.png) |
| P02 | 1600×900 | Windows 当前 150% | 2400×1350 | [Windows 当前 DPI](MS-03_P02_1600x900_Windows当前DPI.png) |

确定性矩阵使用 Qt offscreen 平台明确控制逻辑尺寸与缩放因子；Windows 当前 DPI 截图单独使用默认 Windows 平台，证明本机 150% 缩放下的真实运行结果。完整机器可读数据见 [截图清单](MS-03_截图清单.json)。

## 2. 视觉对比

对比图位于 `docs/milestones/MS-03/evidence/ui-alignment/comparison`：

- P02：HTML 基线、修复前 Qt、修复后 Qt 三联图；
- P04：HTML 基线与修复后 Qt 并排图；
- P07：HTML 基线与修复后 Qt 并排图。

差异分级、关闭情况和剩余 P2 平台渲染差异见 `docs/milestones/MS-03/MS-03_UI差异报告.md`。

## 3. Qt Designer 与运行时

六个生产 `.ui` 文件继续由 `qt_wrap_ui` 编译。应用外壳和动态页面通过 C++ 布局管理器装配，未使用大量绝对坐标。图谱 Canvas、视口联动和业务命令与 Designer 静态布局保持解耦。

构建和安装树部署匹配配置的 Qt Core/Gui/Widgets DLL、Windows/offscreen 平台插件和 `qt.conf`。清空 Qt 插件环境变量后仍可由默认 Windows 平台启动。

## 4. 自动截图与对比

```powershell
pwsh -NoProfile -File .\scripts\capture-ms03-ui.ps1

pwsh -NoProfile -File .\scripts\compare-ms03-ui.ps1 `
  -Baseline .\docs\baseline\Signal-Studio-Dev-Docs\02_原型设计\页面截图\标准截图\SS-P02-1600x900.png `
  -Before .\docs\milestones\MS-03\evidence\ui-alignment\before\MS-03_工作台_1600x900.png `
  -After .\docs\development\ui-preview\MS-03_P02_1600x900_100百分比.png `
  -Output .\docs\milestones\MS-03\evidence\ui-alignment\comparison\MS-03_P02_基线_修复前_修复后.png
```

截图脚本检查进程退出码、文件存在性、物理像素和 DPR，并输出 JSON 清单。功能结论来自具名 CTest、安装消费者和默认 Windows 平台启动回归，不只依赖截图。

## 5. 最终截图摘要

| 文件 | SHA-256 |
|---|---|
| `MS-03_P02_1280x720_100百分比.png` | `b93bd7808939dbba38587be9bb61375603809c41e72c12642c0165c8192a6e09` |
| `MS-03_P02_1600x900_100百分比.png` | `da4d2648816202cede3bb38d24a8e9e2a8debdc2bda2b850f5ac0deabd8497d6` |
| `MS-03_P02_1920x1080_100百分比.png` | `e360c5d1781e782261ddfc3f6603a2fd1246e26d2650ba313a2f3f932003e7f8` |
| `MS-03_P02_3840x2160_100百分比.png` | `dcebc1cb3e265f79a8809a973ebb9774dd03851252d307198b40d74f483cc583` |
| `MS-03_P02_1920x1080_150百分比.png` | `6ac9a7effeb4f9f24734ee2226e7550ac9a6d0f81b5ee9ec13d0c6c829c377c7` |
| `MS-03_P02_1920x1080_200百分比.png` | `71ab58100d0c0ab348652d10b6ee15e79fbf05b8eba019702d748a33abbd1046` |
| `MS-03_P04_1600x900_100百分比.png` | `33326809bd880a3d9bf3532c701ec714e7dfc3938e7ff417c1eebbf281844f1c` |
| `MS-03_P07_1600x900_100百分比.png` | `10cc4ab0b91f645f4c6ac731a5c2e790d4f3e816600d0f423582439995976457` |
