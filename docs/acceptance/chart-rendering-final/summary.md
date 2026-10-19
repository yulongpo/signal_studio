# 图谱显示修正验收

日期：2026-10-09；分支：`rebuild/a143-native`。

宽带和窄带时域值按 ADC 计数等效值绘制；宽带与窄带均提供包络。每次切换波形模式默认依据当前可见数据自动适配，使曲线占绘图区高度约 75% 并垂直居中。手动调整幅值轴后保留用户范围，直至再次自动适配。高倍缩放时逐样本绘制点标记，并在悬停提示样本序号、时间和波形值。窄带分析页移除了顶部导航窗格；所有图谱显示坐标刻度、数值及物理单位，窄带图谱网格可独立开关。

Release 隔离输出构建链接成功。Core、窄带通道处理、显示抽取和 Qt UI 四个测试程序均退出成功；Qt UI 为 **69 passed / 0 failed**，其中包含宽带和窄带逐样本点显示、波形自动适配、四页坐标单位和网格开关断言。测试结果按项目约定写入仓库 `out/` 临时目录。

当前 Release 程序在第二块屏幕上完成宽带及窄带原生 GPU 验收：连接序号 2，Redmi 27 NU（`\\.\DISPLAY6`），3840×2160 物理像素、150% DPI、2560×1440 逻辑分辨率。QRhi 后端为 D3D11，设备为 AMD Radeon(TM) 880M Graphics。宽带报告记录 25 次 GPU 数据绘制、5 次顶点上传和成功的纹理复用；窄带四页分别记录 22、132、2、130 次 GPU 数据绘制，整体验收通过。机器报告、屏幕清单及两张截图保存在本目录。

原生 GPU 验收运行了隔离 Release 输出目录里的当前程序，运行时从既有 Release/Qt 路径解析动态库。这证明当前源码的屏幕和 GPU 绘制路径；它不作为 system-only PATH 的独立部署包验证。Debug 源码编译完成，但链接被本机 MSVC `LNK1101: MSPDB140.DLL` 版本不匹配阻断。该本机链接问题与源码编译诊断分开记录。

```text
wideband-release-report.json
narrowband-release-report.json
screen-inventory.json
wideband-release.png
narrowband-release.png
```
