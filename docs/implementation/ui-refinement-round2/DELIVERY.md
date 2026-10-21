# 第二轮 UI 优化交付

图谱设置、窄带四页与工程树优化已在`rebuild/a143-native`完成。设置靠各图标题栏右侧，宽度不足时进入平铺“更多设置”；窄带观察页保留波形、PSD、时频三图，PSD N可以独立设置；工程树突出名称、短物理摘要及当前对象。

按用户后续要求，只接受当前v6工程，移除v1–v5导入迁移与升级备份；“打开旧版工程JSON”已改为“打开工程JSON”。停止新增测试，之前已完成的检查保留，过期迁移用例随功能移除。

## 程序与操作

[Release程序](/D:/project/signal_studio_rebuild/out/vs2026-qt611-release_bin/SignalStudio.exe)和[Debug程序](/D:/project/signal_studio_rebuild/out/vs2026-qt611-debug_bin/SignalStudio.exe)位于独立部署目录。

1. 窄带观察页的PSD标题栏选择“统计谱”，直接设置PSD N；点击时频图驻留后切为帧谱，N明确显示随STFT，切回统计谱恢复原值。
2. 其它谱参数优先在所属图标题栏调整，剩余参数直接在“更多设置”中编辑。外部点击、Esc、上下文切换等关闭规则保留，内部下拉不会误关整层。
3. 观察页上下比例可以拖动并保存；合成星座与眼图位于调制分析页，识别页配置滚动而运行/停止固定可见。未运行不显示伪造完成指标，显示处理状态与识别反馈分开维护。
4. 工程树的文件/通道名称优先，长名称省略、完整内容保留Tooltip；当前对象、多选、焦点和悬停分开呈现。来源颜色、按需加载、双击定位、Ctrl/Shift选择和刷新保持继续可用。

完整变更见[CHANGELOG.md](CHANGELOG.md)，需求对应、命令及失败迭代见[VALIDATION.md](VALIDATION.md)。

## 回归与证据

| 项目 | Debug | Release |
|---|---|---|
| 全量CTest | 29/29，840.44秒 | 29/29，235.44秒 |
| 基础UI | Qt108/108，业务106 | Qt108/108，业务106 |
| 既有UX五组 | Qt38/38，业务28 | Qt38/38，业务28 |
| 工程树呈现 | 8项通过 | 8项通过 |
| 最终原生旅程 | 24/24 | 24/24 |
| 宽带GPU / 窄带独立启动 / 软件回退 | 各通过 | 各通过 |

基础UI的480秒保护不变，最终CTest UI耗时471.92/174.92秒；这些是当次共享主机运行记录，不代表性能基准。每组Qt初始化/清理计数不计业务用例，也不与原生场景数相加。

全量回归对应产品提交`1176f8c`。之后仅更新当前工程入口文案、捕获程序的连续稳定等待及构建身份，最终以`e666cea38bad6918f8fe8bbbd005025c7761496f`重新构建；交付程序再执行原生操作与部署核对。程序SHA、命令、报告与截图索引见[evidence/source-identity.json](evidence/source-identity.json)。

最终两配置原生各24/24。真实CPU DDC均输出7680个样本，保存重开后SHA一致，独立PSD N8192保持。Release正式FFSCN旅程43/43，实际TensorRT10.11.0.33在RTX5060 Laptop GPU完成3/3窗口，并验证16384个CPU DDC输出样本及重开。该输入为已有TorchSig CF32文件，不外推真实RF精度。

原生屏幕为当次唯一4K/150%外接屏Redmi 27 NU / 连接2 / DISPLAY6，PNG物理3840×2160、全屏逻辑2560×1440；报告保留完整Windows原生路径、物理尺寸和DPI。显示为QRhi D3D11 / AMD Radeon 880M，推理GPU另行记录。没有执行1366×768工作。

[窄带观察](screenshots/narrowband-observe.png)、[PSD更多设置](screenshots/narrowband-psd-more.png)、[驻留帧PSD](screenshots/narrowband-frame-psd.png)、[调制与眼图](screenshots/modulation-eye.png)、[识别空态](screenshots/recognition-empty.png)、[工程树多选](screenshots/project-tree.png)、[真实检测通道DDC](screenshots/27-real-algorithm-cpu-ddc.png)。

[Debug原生报告](evidence/native-debug.json)、[Release原生报告](evidence/native-release.json)、[正式模型报告](evidence/production-release.json)保留全部91个原始场景路径。精选PNG为原始字节副本，[截图索引](evidence/screenshot-index.json)记录归档映射与SHA-256。全部原始文件位于`out/tasks/ui-refinement-round2/05-acceptance/`，旧失败报告未覆盖。

## 保持的边界

真实IQ处理、正式检测模型前向和CPU DDC分别记录。窄带星座、识别和解调仍是明确标示的合成示例；批量G/T及真实RF精度仍由用户后续手动验证，没有新增精度或Ubuntu运行结论。

只普通推送`origin/rebuild/a143-native`；不修改main、不创建PR。发布核对保存在`out/tasks/ui-refinement-round2/05-acceptance/diagnostics/publish-verification.json`。
