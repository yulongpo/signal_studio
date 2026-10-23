# a143-native 宽带 UI 优化交付

W0–W6 已完成。图谱工具栏、临时设置层、右侧四页和任务操作已经在 `rebuild/a143-native` 实现；最终两配置回归和实际 4K/150% 原生旅程全部通过。产品及验证工具源码为 `399b85e970f2f22432cd95277dc30e9075ca4cb4`，本次交付文档在随后提交中归档。阶段提交、基线与发布规则见 [EXECUTION_STATUS.md](EXECUTION_STATUS.md)。

## 运行与操作

推荐从 [Release 输出程序](/D:/project/signal_studio_rebuild/out/vs2026-qt611-release_bin/SignalStudio.exe) 启动；[Debug 输出程序](/D:/project/signal_studio_rebuild/out/vs2026-qt611-debug_bin/SignalStudio.exe) 也已验证。两者均从各自部署目录、仅系统 PATH 独立启动通过。源码 clone 不包含 out 中的模型和部署 DLL；本机现有验证包包含这些运行资产。

1. 创建或打开工程，再打开信号进入宽带。辅助图标题栏显示波形分量或 PSD 常用项；主图标题栏提供模式、STFT N、唯一色表、电平、显示和设置。
2. PSD/STFT 高级参数、电平和显示在临时层中调整。同入口再次点击、外部点击、Esc、应用失焦或切换上下文会按规则关闭；内部下拉不误关。右侧常驻属性栏仍手动收起。
3. 右侧使用显示、检测、标记、通道四页。检测页首屏展示模型、输入谱、范围、阈值、时间定位和短预检；专业/后处理/资源及计算/诊断按需展开，运行区固定。
4. “宽带检测”仅打开配置；“检测当前区域”明确使用当前视区；ROI Enter 确认精确范围，Esc 恢复原工具。查看历史来源/详情不覆盖同文件草稿，派生复制历史快照及冻结范围而不自动运行。
5. 任务行提供来源、详情、派生和取消。列表聚焦后 Enter 查看来源、Alt+Enter 详情、Ctrl+Enter 派生、Ctrl+Shift+X 取消选中任务；文件 B 的属性栏不能模糊取消文件 A。

参数保持 Enter、Tab/Shift+Tab 或应用内点击提交；输入中、后台刷新和应用失焦不提交。非法值保留错误与可恢复文本，Esc 撤销，切文件/通道/工程撤销草稿。方法切换恢复最近已提交合法值，PSD/STFT 方法历史独立且不扩展工程 v6。完整行为见 [UX_SPEC.md](UX_SPEC.md)，变更见 [CHANGELOG.md](CHANGELOG.md)。

## 最终验证

| 项目 | Debug | Release |
|---|---|---|
| 全量 CTest | 28/28，601.04 秒 | 28/28，114.75 秒 |
| 基础 UI | Qt 108/108，业务 106 | Qt 108/108，业务 106 |
| 新增 UX | 业务 26，五组 Qt 36/36 | 业务 26，五组 Qt 36/36 |
| 新 UI 原生场景 | 57/57 | 57/57 |
| 正式 FFSCN / SCN | 3/3、2/2 窗口 | 3/3、2/2 窗口 |
| 真正键盘取消 | Cancelled，24/1998 完成窗口保留 | Cancelled，30/1998 完成窗口保留 |
| 标记→真实 CPU DDC→保存重开 | 8192 样本、输出 SHA 一致 | 8192 样本、输出 SHA 一致 |
| 宽带 GPU / 窄带独立部署 / 软件回退 | 各通过 | 各通过 |

Release 还通过正式来源兼容旅程 43/43，以及缺模型、受控无兼容 GPU 两个独立复制包异常场景。两配置全量测试均无失败；基础 UI 480 秒保护保持不变，最终 UI CTest 分别 416.02 / 71.58 秒。Qt 初始化/清理不计业务用例，原生场景数不与业务用例数相加。

原生验收在当次唯一实际外接屏 Redmi 27 NU / 连接2 / DISPLAY6，3840×2160、150%、逻辑2560×1440全屏进行。截图与实际点击均检查；1920×1080仅离屏，没有执行1366×768工作。三个显示图谱为 D3D11 / AMD Radeon 880M，正式前向为独立 TensorRT10.11.0.33 / RTX5060 Laptop GPU，CPU DDC 单独记录。

检测请求/worker协议、物理时间频率、三来源标记、来源修订、通道快照及工程v6保持兼容。全量存储测试逐一验证v1–v5迁移前原始字节备份、失败原子性及v6关系/快照，原生验证保存重开。全部要求逐项证据见 [ACCEPTANCE_MATRIX.md](ACCEPTANCE_MATRIX.md)，命令、失败迭代及最终日志见 [VALIDATION.md](VALIDATION.md)。

## 截图与证据

[检测首屏](screenshots/after-detection.png)、[PSD设置层](screenshots/psd-settings.png)、[任务详情](screenshots/task-details.png)、[通道与DDC](screenshots/channel-ddc.png)、[保存重开](screenshots/saved-reopened.png)、[缺模型](screenshots/missing-models.png)、[无兼容设备](screenshots/no-compatible-gpu.png)。[改造前布局](screenshots/before-layout.png) 来自 W0 演示数据，与最终 CF32 样本不同，只用于查看布局，不作算法效果对比。

原始报告 [Debug](evidence/native-debug.json)、[Release](evidence/native-release.json)、[正式来源兼容](evidence/production-release.json) 保留全部场景路径和运行记录。精选 PNG 为原始字节副本；[screenshot-index.json](evidence/screenshot-index.json) 记录场景、原路径、归档路径及 SHA-256。完整场景和工程留在 `out/tasks/wideband-ux-optimization/a143-native/W6/`，旧失败及中断未覆盖。

[source-identity.json](evidence/source-identity.json) 记录源码与程序哈希。已验证的程序在W6代码提交前构建，嵌入版本戳为afefd73；实际验证内容对应399b85e，随后只归档文档。Release 主程序 SHA-256 为 `6fea554fa189c868440d5ec3266b9149be8dd1195c2d8c6cfaa07e68c621ce01`，Debug 为 `84a43081e809acbbbcbb45124146ad511aece24ecc51375265e2bf232de0c8ec`。

## 发布与验证边界

本轮只推送 `origin/rebuild/a143-native`，不修改main、不创建PR、不整体合并grok。本地阶段提交为 W0 0f67455、W1 8db0514、W2 bf2924e、W3 77ec6b5、W4 137589b、W5 afefd73、W6 399b85e，之后是本目录交付文档提交。实际推送退出码、本地/远端SHA核对记录在 `out/tasks/wideband-ux-optimization/a143-native/W6/diagnostics/publish-verification.json`，最终推送HEAD以该记录及Git远端为准。

本轮代表性输入为用户已有TorchSig CF32文件，证明真实文件处理及正式模型前向。批量G/T矩阵仍由用户手动执行，未新增真实RF精度结论；也未新增全配置压力/性能或Ubuntu验收。当前TensorRT/RTX5060 SM12.0部署包只证明本机组合。窄带星座、识别和解调仍保留合成示例标识。
