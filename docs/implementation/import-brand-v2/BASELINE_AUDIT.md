# Import A1.2 / Brand v2 Baseline Audit

## M0 自查

- 基线：`8d0564a`，分支 `rebuild/a143-native`；2026-10-10 执行 status、branch、log、fetch --prune。工作树干净，HEAD 与 origin 同分支一致。保留用户的 .gitignore 提交。
- 当前用户明确授权每节点本地提交、全部完成后正常推送；不改写历史。
- 依据：当前 AGENTS.md、任务全文、A1.2 交互说明/HTML/五张参考图、Brand v2 README/QRC/RC/SVG。任务目录被忽略，不参与构建。
- 工程：C++20 / Qt 6.11.1 EXACT / VS 2026；Widgets/QRhi D3D11，现有软件回退。

## 调用与状态链

`MainWindow::showAddFileDialog -> describeInt16IqFile -> Session::addDemoFile(metadata) -> SourceLoader -> pollSourceLoads -> PlotWidget workers -> Int16IqFile -> spectral_analysis`。

窄带链：`Session::createChannel -> ChannelSampleCache -> processChannelSamples -> Int16IqFile -> DDC/FIR/rational resampling`。项目链：`ProjectStore::load -> 完整候选校验 -> Session::replaceProject -> 按保存前缀扫描`；保存使用 QSaveFile。项目 v3 的索引是 uint64 十进制字符串。

## 影响与兼容

扩展 domain 格式字段，基础设施 reader/loader/persistence/cache identity；替换旧 470px 导入框，保留主窗口图谱/手势/谱估计。旧 CI16 样本默认除以 32768，波形恢复 ADC 计数；新格式必须明确幅度换算。实数轴 0..Fs/2，Fc 仅作参考；禁用实数复数 DDC。完整帧计算严格拒绝尾数，不隐藏丢弃。

后台请求沿用项目代次、源 ID、配置 key；导入独立队列先校验/扫描，再提交有效源，取消保留完整前缀。读取块接近 4MiB、预览小于 32MiB、导航最多 2048 点，禁止整文件堆分配。

主要风险：Planar 与通道分块地址组合、浮点非有限值、旧归一化语义、过期回调、停止后重开、格式缓存污染。分别增加字节矩阵、数值、持久化和取消测试。旧 v1/v2/v3 回填 CI16LE/IQ/单通道/零偏移，v4 完整记录格式。

## UI 与品牌

A1.2 布局保留页头、三页导航、快速导入文件条、两列可滚动表单/预览、PSD 上方、波形与散点/直方图并排、STFT 下方、固定底栏；加载使用居中遮罩卡及原型三个阶段。批量保留表格/右侧模板/当前文件编辑。模板独立页。

Brand v2 正式复制进 resources/branding，QRC 路径自包含，共享资源模块供 app/tests/capture；ICO 嵌入 EXE。欢迎 lockup、标题 compact、导入 compact、窄带 compact、关于 lockup。禁止更改主图 LUT/功率/游标或覆盖数据。

## 验收边界

原生仅第二连接屏 Redmi 27 NU / DISPLAY6 / 3840x2160 / 150% DPI 全屏验收，记录实际驱动/upload/draw。Offscreen 不证明 GPU。旧报告不作为本轮通过证据。Debug/Release 构建、CTest、独立包硬件/软件、九张 Qt 截图和报告均在 M5 重跑。
