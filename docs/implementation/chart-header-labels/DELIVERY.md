# 谱图切换与参数标识交付

本轮已完成：谱图类型切换固定在左侧，参数及操作靠右；宽窄带参数、眼图和右侧显示摘要均有明确名称及单位。长数值按两位小数展示，编辑保留完整精度；控件与标签共同进入平铺“更多设置”。另外修复了窄带字段折叠后跨页漏绑的问题。

最终程序源码为 **`696ce12d689afd2be849c9b72999e8f12778587e`**。源自 `55af263`，依次提交 `f9f05bb`（切换、标签、显示精度）、`a64117e`（窄带宿主与页面路由）、`696ce12`（右侧摘要名称及单位）。后续提交只归档交付材料。

[Release程序](/D:/project/signal_studio_rebuild/out/vs2026-qt611-release_bin/SignalStudio.exe) · [Debug程序](/D:/project/signal_studio_rebuild/out/vs2026-qt611-debug_bin/SignalStudio.exe)

## 结果与证据

| 检查 | Debug | Release |
|---|---:|---:|
| 当前源码配置、构建与Qt/CRT部署 | 通过 | 通过 |
| 既有相关UI业务用例 | 19/19（Qt21/21） | 19/19（Qt21/21） |
| 谱参数、图谱、布局、设置层CTest | 4/4（业务19项，Qt27项） | 4/4（业务19项，Qt27项） |
| 最终原生交互场景 | 24/24 | 24/24 |
| 宽带GPU独立启动 | 通过 | 通过 |
| 窄带四页GPU独立启动 | 通过 | 通过 |
| 软件显示独立启动 | 通过 | 通过 |

相关Qt回归对应 `a64117e` 的实现；其后的 `696ce12` 仅修正显示摘要文案。最终两配置原生、六项独立启动和补充精度截图均使用 `696ce12` 的实际交付程序。没有新增测试用例或捕获模式。Qt初始化/清理计数不计入业务用例数；本轮没有重跑全部29个CTest目标或综合UI全部106个业务用例。480秒UI保护保持原值。

原生使用当次唯一4K/150%外接屏：**Redmi 27 NU、连接序号2、DISPLAY6**，物理3840×2160、逻辑2560×1440全屏。报告保留原生设备路径、物理DPI、逻辑DPI及实际设备像素比。1920×1080仅由既有离屏布局检查覆盖；没有执行1366×768工作。

两配置均完成真实CPU DDC、独立PSD N8192往返及v6保存重开，DDC均输出7680个样本，SHA-256均为 `6fd3ba52da78ac48e3d514cb2b7b4adf87674793451331a86287d91a862efe3c`。GPU显示为QRhi D3D11 / AMD Radeon 880M；软件结果单独标示。调制、眼图、识别和解调仍为标示的合成示例。本轮未重新执行模型前向、批量G/T或真实RF精度验证。

## 可直接审查的界面

[长数值、长窗名及右侧摘要](screenshots/precision-and-labels.png)显示“参考 −79.01 dBFS/Hz”“动态113.81 dB”和完整的Blackman-Harris。输入实际值为 −79.0121397394、113.814300793，截图前后工程文件SHA一致。

[窄带观察](screenshots/narrowband-observe.png) · [PSD更多设置](screenshots/narrowband-psd-more.png) · [驻留帧PSD](screenshots/narrowband-frame-psd.png) · [眼图设置](screenshots/eye-settings.png) · [宽带PSD](screenshots/wideband-psd.png) · [STFT更多设置](screenshots/wideband-stft-more.png) · [图谱最大化](screenshots/wideband-maximized.png) · [侧栏收起](screenshots/sidebar-collapsed.png)

九张归档PNG均为原始字节副本；[截图索引](evidence/screenshot-index.json)记录源路径和SHA-256。[程序及源码身份](evidence/build-identity.json)、[验证摘要](evidence/verification-summary.json)、[Debug原生报告](evidence/native-debug.json)、[Release原生报告](evidence/native-release.json)及同目录原始Qt日志可用于核对。

## 构建与复现

使用公共 `vs2026-qt611-debug` / `vs2026-qt611-release` presets，Qt6.11.1 EXACT。Debug最终采用：

```powershell
cmake --build --preset vs2026-qt611-debug --parallel 4 -- /p:LinkIncremental=false
```

曾有Debug增量链接产物在进入main之前的CRT初始化阶段发生访问异常；[实际调用栈](evidence/debug-startup-stack.txt)与[重链日志](evidence/debug-capture-relink.log)保留，源码未添加规避。完整重链后既有参数检查、最终24场景及独立启动通过。

原生复用 `scripts/screenshot.ps1 -Configuration <Debug|Release> -UiRefinement -OutputDirectory <新目录>`；独立启动复用 `scripts/verify-package.ps1` 的GPU、`-Narrowband`、`-SoftwareRenderer`入口，均清除开发环境路径和DPI覆盖。该捕获器保留既有 `ui-refinement-round2` 模式名和24场景计数，本轮结果存入独立目录。

完整中间记录在 `out/tasks/chart-header-labels/01-controls/`、`02-acceptance/`；最初Qt文案/入口失败、窄带宿主诊断、原生GPU稳定性失败及CRT启动异常均保留。[实施说明](IMPLEMENTATION.md)记录归因及处理。本轮只接受当前v6工程；此前未跟踪输入和前两轮报告保持原样。

按已授权流程普通推送同名远端分支；未修改main，不创建PR，不合并grok分支。最终远端SHA在推送后核对，发布记录位于本轮本地任务目录。
