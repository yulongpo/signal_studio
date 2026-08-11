# MS-4.5 实施记录

## 仓库预检

- 执行 `git fetch --all --prune` 成功；
- 分支为 `codex/full-signal-studio-development`；
- origin fetch/push 均为 `https://github.com/yulongpo/signal_studio.git`；
- 实施前已审计分支最新代码、工作树和前序验收；本里程碑基线提交为
  `58ab7474a102f0e4edee2000872b1e36596be8c3`。

## 前序与后续边界

- MS-02 提供 oneMKL/cuFFT FFT、VSL 卷积、LAPACKE、libsamplerate、ProcessingChain、
  取消、缓存与后端来源；本里程碑只编排和扩展参数，不复制内核。
- MS-03 提供 DisplayMapping、AtomicFrameCoordinator、ViewRequestId、可见性和 Qt
  Designer Workbench；本里程碑扩展参数编辑和绑定。
- MS-04 提供工程、导入、基础分析、TaskRuntime 和 Artifact；本里程碑把固定分析改为
  参数闭环并补齐恢复语义。
- MS-05 保留 Selection 建通道、DDC、重采样输出和多通道联动；MS-06 保留插件、模型、
  数据集和调制解调；MS-07～MS-09 保留发布工程化、第二应用和最终发布验收。

## 依赖审计

没有引入新依赖。FFT 使用 oneMKL DFTI/cuFFT，FIR/卷积使用 VSL，IIR 和
Savitzky-Golay 求解使用 LAPACKE，重采样使用 libsamplerate。MS-4.5 新代码只负责参数
校验、算法编排、缓存身份、状态、序列化、UI 和来源追溯。

## 实施收口

1. 建立版本化参数快照、校验、规范化哈希、结果单位和完整后端 provenance。
2. 参数化 Spectrum/PSD/STFT、八种窗、Periodogram/Welch、平均/最大保持、平滑和
   ProcessingChain 预滤波。
3. 建立单请求变换缓存与平滑缓存，maximum-hold 会话聚合不写回缓存；显示与测量来源
   变化最小失效。
4. 建立 Qt Designer 基础/高级面板、预设、派生信息、DPI 布局和异步滤波预览。
5. 建立工程扩展保存、旧工程迁移、同主版本损坏拒绝、未来主版本拒绝和事务恢复。
6. 建立 Artifact 参数哈希、算法、后端/设备/精度、当前范围、完整 lineage 和任务来源。
7. 正式任务采用取消/提交 CAS 状态机；提交在最新视图许可内完成设置、Artifact、
   Workspace，并把三个真实 Artifact 文件原子登记到 TaskRuntime 后才密封完成。
8. 工程打开时核对 TaskRuntime 历史，清理未完成或缺少完整已登记文件的正式结果；损坏
   Artifact 在重启验证中使任务失败。

## 最终验证与复审

2026-08-11 最终代码验证：CPU Debug/Release 全量分别 267/267（411.54/284.80 秒）；
CPU/CUDA Debug/Release 专项均 24/24（70.41/12.79/39.55/12.29 秒）。14 个变更 C/C++
文件通过格式检查，7 个变更生产翻译单元 clang-tidy 为 0 errors；批准基线差异为 0。

首轮独立规格与代码质量复审发现的 Important 项已经整改并回归。最终快照再次请求独立
复审时受代理额度限制，未获得新的外部结论；主代理完成逐项规格和代码质量复审，结论为
无剩余 Critical/Important。用户要求不再运行 30 分钟稳定性测试，因此只保留并审计
2026-07-30 历史证据。MS-05 未开始。
