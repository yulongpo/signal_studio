# MS-03 开发报告

MS-03 把 Visualization 与 Workbench 从包边界升级为真实可复用平台。公共层只使用标准 C++20 和既有 Signal Studio 类型；Qt 私有层提供时域、PSD、STFT、频谱、星座、眼图、导航、Selection、测量、截图、工作台 Dock 和中心面板。

生产构建实际消费六个 Qt Designer `.ui` 文件。平台库默认显示真实空状态；演示程序显式注入演示帧与工作台内容。构建和安装均部署匹配配置的 Qt DLL、Windows/offscreen 平台插件与 `qt.conf`，关闭插件环境变量后仍可由默认 Windows 平台启动。

完整实施细节见 [MS-03_实施记录.md](MS-03_实施记录.md)，界面成果见
[MS-03 UI 预览索引](../../development/ui-preview/MS-03_UI预览索引.md)。
