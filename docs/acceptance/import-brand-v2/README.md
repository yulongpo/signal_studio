# Import A1.2 / Brand v2 Native Acceptance

2026-10-11，Windows x64 / Qt6.11.1 / VS2026。最终Debug和Release配置、构建、部署、CTest各7/7；Qt UI各87 passed、0failed、0skipped。`summary.json`是机器摘要，`build-*.log`和`ctest-*.log`保留原生输出；`ui-*.txt`为QtTest记录，不用浏览器原型测试代替。

第二连接显示器：Redmi27NU / `\\.\DISPLAY6` / 3840x2160物理 / 2560x1440逻辑 / DPR1.5。硬件QRhi D3D11 / AMD Radeon(TM)880M Graphics，真实顶点/纹理上传和数据draw通过。独立包脚本从输出目录以系统PATH运行，GPU与软件分别记录，软件不声称硬件加速。详见package-*.json及gpu/*。

## Screenshots

下列均为真实Qt Widgets原生捕获，不是HTML截图。主窗口在第二屏实际全屏；导入/关于图裁切真实模态窗口，不称整屏。每图screen/DPR/input/nonBlackSamples保存在`native-capture-report.json`。

| 文件 | 场景/输入 | 物理像素 |
|---|---|---|
| [01-welcome-brand.png](screenshots/01-welcome-brand.png) | 欢迎与工程入口 | 3840x2160 |
| [02-broadband-brand.png](screenshots/02-broadband-brand.png) | 实际CI16宽带，原包16384复样本 | 3840x2160 |
| [03-import-complex.png](screenshots/03-import-complex.png) | A1.2复数配置，真实CI16预览 | 1815x1290 |
| [04-import-real.png](screenshots/04-import-real.png) | RI16真实单边PSD/波形/直方图/STFT，16384实样本 | 1815x1290 |
| [05-import-preview-real-file.png](screenshots/05-import-preview-real-file.png) | 已选原包CI16实际文件预览；与03同一正确稳定状态 | 1815x1290 |
| [06-import-batch.png](screenshots/06-import-batch.png) | CI16/RI16混合队列、逐源格式与状态 | 1815x1290 |
| [07-import-progress-partial.png](screenshots/07-import-progress-partial.png) | 128MiB实际CI8临时RAW停止，4,194,304完整前缀 | 1815x1290 |
| [08-narrowband-brand.png](screenshots/08-narrowband-brand.png) | 内置真实IQ源CPU DDC；其它Demo仍标明 | 3840x2160 |
| [09-about-brand.png](screenshots/09-about-brand.png) | 品牌、0.1.0版本、构建基准/Qt/许可与复制 | 810x600 |
| [10-import-templates.png](screenshots/10-import-templates.png) | 原型三列模板卡与保存/导出 | 1815x1290 |

导入逻辑尺寸1210x860；主窗实际逻辑2560x1440。没有进行其它尺寸/DPI验收。07临时源已删除；测试包原始CI16/RI16在tests/fixtures可复现。具体命令和状态边界见[执行报告](../../implementation/import-brand-v2/CODEX_EXECUTION_REPORT.md)。

## Evidence

- `brand-debug/release.json`：原包SVG与PE多尺寸ICO，PNG回退另有Debug独立包验证。
- `script-fixtures*.txt`：432矩阵、72DDC、独立脚本SHA与384参考数值。
- `load-debug/release.json`：64MiB真实只读扫描的Windows峰值工作集与时间；不是射频持续吞吐、GUI或GPU内存结论。
- `package-release-narrowband-*.json`：四窄带页全部settled，GPU逐页draw非零；软件单独通过。
- `gpu/`：真实第二屏全屏GPU图、屏幕清单和driver证据。该smoke使用演示工程，实际IQ数值由独立reader/谱/DDC测试及02..05证明。

未验证项：Ubuntu/Vulkan、多GB持续吞吐、Windows shell图标缓存视觉；未实现且明确禁用：实数解析DDC和不支持的SigMF变频段。基线合成识别/解调/星座/眼图仍为Demo。详见执行报告第7节，不将这些项目写成通过。
