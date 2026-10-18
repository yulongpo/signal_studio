# U4 综合验收

分支 `rebuild/a143-native`，实现基线 U3 `c182b3e`。继续使用项目现有
构建、CTest、截图和独立部署入口；本节点不修改模型、算法或 worker
协议，不运行大量 IQ 效果矩阵。以下均为本节点实际完成结果。

| 检查 | 结果 | 证据 |
|---|---|---|
| Debug 配置、构建、完整 CTest | 23/23，合计 518.20 秒 | [配置](logs/debug-configure-r1.txt)、[构建](logs/debug-build-r1.txt)、[CTest](logs/debug-test-r1.txt) |
| Debug 完整 Qt UI | 108/108，0 失败、0 跳过，366.142 秒；CTest UI 370.81 秒 | [Qt 日志](logs/ui-debug.txt)、[JUnit](logs/ui-debug.txt.xml)、[逐项计时](logs/ui-debug.txt.timing.jsonl) |
| Release 配置、构建、完整 CTest | 23/23，合计 126.36 秒 | [原始日志](logs/release-all-r1.txt) |
| Release 完整 Qt UI | 108/108，0 失败、0 跳过，68.338 秒；CTest UI 69.06 秒 | [Qt 日志](logs/ui-release.txt)、[JUnit](logs/ui-release.txt.xml)、[逐项计时](logs/ui-release.txt.timing.jsonl) |
| Debug / Release 原生模型旅程 | 43/43、46/46；真实 FFSCN、CPU DDC、保存重开 | [Debug](native/native-debug-r1/wideband-detection-report.json)、[Release](native/native-release-r1/wideband-detection-report.json) |
| Release 原生标记 U1 / U2 / U3 | 8/8、10/10、12/12；实际新屏、同一最终实现 | [选择](native/native-release-u1-r1/annotation-interactions-report.json)、[编辑](native/native-release-u2-r1/annotation-interactions-report.json)、[通道](native/native-release-u3-r1/annotation-interactions-report.json) |
| 独立部署及显示回退 | 两配置 × 宽/窄带 × GPU/软件，共 8 模式；每模式 package 与 smoke 均通过 | [运行摘要](validation-summary.json)、[执行记录](diagnostics/native-stage-ledger.json)、`renderer/` |
| 推理不可用时的历史操作 | 缺少模型、无兼容 CUDA 设备 2/2；标签、手动及历史仍可用 | [缺模型](availability/missing-models/wideband-availability-report.json)、[无设备](availability/no-compatible-gpu/wideband-availability-report.json) |

Qt 使用原有完整 runner，480 秒保护不变，没有删减或拆分场景。
offscreen / QWidget 回退的交互结果与后续原生 GPU 结果分别记录。

进入 U4 时，Debug 的 45 项输入与 26 个程序均与 U3 最终身份一致；
重新配置、构建后，三个 UI 程序身份改变。因此本节点重新执行完整
Debug 回归，没有把旧结果套到新程序。具体见
[构建前](diagnostics/debug-reuse-before.json)和
[构建后](diagnostics/debug-reuse-after.json)的身份比较。

两配置构建完成后冻结 237 项构建输入、52 个程序及 95 项运行库和
模型资产，共 384 项。完整回归及原生验收后逐项复核均未变。
[冻结身份](diagnostics/input-freeze.json)及
[复核结果](diagnostics/input-freeze-verified.json)记录字节数与 SHA-256。

M0-P 的 22 项文件、获批 U0 原型、检测实现和 worker 协议保持不变，
见[冻结契约核对](diagnostics/frozen-contracts.json)。U0 是产品交互依据，
其浏览器演示和检查不作为 Qt、模型或 DSP 证据。

## 原生与性能边界

本轮原生使用当前外接 U27U2D / DISPLAY2、连接序号 1，实际
3840×2160、150% DPI、2560×1440 逻辑全屏；未固定旧屏身份。
逐项与获批 U0 对照见[原生验收](NATIVE_ACCEPTANCE.md)。

Debug/Release 真实 FFSCN 均完成 Sample 与 ROI 各 3 个窗口，
实际算法对象可在待判断、未确认段状态创建通道。两配置本次 DDC
均生成 16384 个有限、非零能量样本，SHA-256 为
`2f5c8ce025ce4a7e7040d23e6ab93ccf8255e5ce334eeef7d18611d97d54bece`；
保存重开保留创建快照，绑定几何变化显示差异且不重算。
Release U3 另有 LocatedSegment、UserRange 和可用前缀尾部的三次
真实 CPU DDC，输出分别为 8000、8192、8192 个样本。

Release 模型启动反馈 134.9452 ms；最终受控任务的启动反馈
113.0498 ms、取消反馈 157.9721 ms、取消完成 179.2669 ms，
均包含实际可见状态的首次绘制，取消保留精确已发布前缀。
十万合成活动段（1000 对象 × 100 段）的 20 次选择 P95 为
84.2224 ms（含图帧），筛选 P95 为 70.4745 ms（含首次绘制，
不含图帧）。这些是本机功能和响应测量，不是 IQ 效果矩阵。

Debug 模型启动反馈为 566.3617 ms，未达 200 ms 建议目标；本轮
未执行 Debug 最终取消/压力，不宣称全部配置性能通过。SCN 的
算法与部署身份未变，本轮不把 FFSCN 旅程当作新的 SCN 效果验证。

GPU 显示为 AMD Radeon 880M / QRhi D3D11，模型为 NVIDIA
RTX 5060 Laptop / TensorRT，CPU DDC 独立记录。package 和 smoke
均从相应 `_bin` 启动并使用 system-only PATH。软件模式明确为
QWidget/QPainter，不代表 CPU 模型推理。原生标记交互夹具不运行模型；
代表性 IQ 是实际读取的 TorchSig 合成基准，不能声称现场 RF 精度。

U3 空视区截图采用受控前缀，真实 SourceLoader 停止及首个未读样本
边界由处理器/Qt 回归单独覆盖。旧 v5 迁移保持原 W: 来源，仅验证
原字节备份、旧快照和重开，不把新本机 DDC 套到旧来源。

历史节点和中间失败记录保持原值。最终规格/质量结论见
[独立复审](REVIEW.md)，文件身份见[产物清单](artifact-manifest.json)。

## 归档自检

[归档自检](diagnostics/archive-selfcheck.json)逐项核对 8 份工程、
31 个标记分片引用、7 份检测 manifest、18 个检测 chunk 和 16 个原始
模型 head 侧文件的路径边界、字节数、SHA-256 及张量长度，全部通过。
正式模型工程副本放入较短的 `projects/debug/` 和 `projects/release/`，
解决 Windows Git 暂存的路径长度限制；[目录映射](diagnostics/project-archive-map.json)
记录原位置及逐字节不变检查。工程内部相对引用及原始报告运行路径不变。
原始 IQ、模型和运行库没有复制到本归档，仍须按记录准备外部输入。
README 和手动入口的 [7 段 PowerShell 命令](diagnostics/document-command-syntax.json)
均经语法解析；这是命令语法检查，不将文档命令当作再次运行的结果。

产物清单覆盖本目录全部文件（清单自身除外），并另记 README、窄带
架构和节点状态的本机字节身份。可用
[归档核对工具](diagnostics/verify_archive.py)的 `verify` 参数只读复核
清单；`check` 会重新检查工程引用及当前本机冻结资产，并更新自检记录，
故重新检查应另存记录，不能覆盖本次交付证据后仍套用旧清单。
