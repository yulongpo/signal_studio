# M3 独立质量审查

首次审查：2026-10-14 00:19；修复复核：00:47、00:58；最终 UI 尾审：01:23（北京时间）。范围为 M2 提交 `cc103e3`、目录整理提交 `9080162` 之后的当前 M3 工作区；仅审查源码、契约、小型自检及主 Agent 的新验证日志，不修改实现、不提交、不执行完整构建或 M4 工作。

最后复核结论：**QUALITY_PASS，仅限当前代码与已核对的验证范围。** 两轮发现及最终未知时间几何框修复均已处理，未发现剩余 P0/P1/P2 质量问题。最后变更后的脚本回归、真实模型 readback 与 25 场景原生截图日志已核对通过；此结论不替代主 Agent 的节点证据归档与本地提交要求，也不提前判定 M4/M5 完成。

## 发现与处理状态

| 级别 | 位置 | 影响与所需验证 | 当前状态 |
| --- | --- | --- | --- |
| P1 | `infrastructure/detection_projection.cpp` 候选构造；`infrastructure/detection_task.cpp` 生产解码 | 原先生产 JSON 丢弃 `peakIndex`，所有候选向 tracker 传入 0；同窗多候选会失败。 | 关闭。四字段生产候选保留原峰索引；每窗两个不同候选的投影 roundtrip 新二进制通过。真实四窗 FFSCN 生成八个对象并精确恢复。Fixture 仍保留三字段契约。 |
| P1 | `infrastructure/detection_model_backend.cpp` `Impl::forward` | 异步传输或 `enqueueV3` 失败早退时，局部 host vectors 可能先于 stream 同步被释放。 | 关闭。`StreamDrain` 在 host vectors 之后声明；live 三个输出头失败注入都保留原错误与 caller output，drain count 增一；同一 backend 随后正常前向通过。 |
| P1 | `infrastructure/detection_task.cpp` `finish` | 原先生产取消/失败只发布空集合，已完成频率观察不可复核。 | 关闭。终态前从已校验 durable prefix 投影；真实 FFSCN 两窗后取消，非空标记、未知时间、完整 eligible denominator 与 exact recovery 通过。未将该取消测试描述为全部设备错误的验证。 |
| P2 | `infrastructure/detection_task.cpp` 最终投影分片循环 | 4096 条与 8 MiB 必须先到为准。 | 代码关闭。逐记录 compact 字节计数，4096 条或 `min(8 MiB-8192, publicationBudget/32)` 先到 flush，单记录超限失败；不再先构造全部 final records。大集合恢复边界见下表。 |
| P2 | `infrastructure/detection_task.cpp` 频率证据扩展；`infrastructure/detection_projection.cpp` 驻留预算 | 频率证据扩展后须重新核对实际投影。 | 代码关闭。附加频率与 window provenance 后按实际 canonical annotation 的四倍字节加每对象 4096 预留复检；tracker 与 window provenance 分摊 projector budget。 |
| P2 | `infrastructure/detection_projection.cpp` `consume` | 恢复网格须核对冻结物理来源。 | 关闭。first 精确绑定 `Fc-Fs/2`，step 精确绑定 `Fs/FFT`；新 bad-grid 测试通过。最终 marker 严格两字段，最终解码也拒绝重复 annotation ID。 |

## 第二轮边界及关闭依据

| 级别 | 位置 | 影响及关闭依据 |
| --- | --- | --- |
| P2 | `infrastructure/detection_task.cpp` `recoverDetectionPublication` | 关闭。取消整集合 saved JSON 累积和旧 8 MiB 总限制，逐条校验、decode 后 move 为 typed annotation。annotation 预留 CPU 请求的一半，最大 shard 另预留 `8*bytes+65536`，剩余分配 projector；不扩大既有请求上限。跨 shard 的全局 ID 去重与严格结束计数仍保留。新 `publicationByteBudgetRoundtrip` 用 80 个 120 KiB 合法合成来源证据、每 shard 12 条、总投影超过 8 MiB，经真实 store/write/recover 后 exact roundtrip；00:54:32 日志通过，对应二进制 00:53:32 晚于源文件 00:53:22。该测试属于序列化边界，不是模型效果或 GPU 验证。 |
| P2 | `domain/scn_detection_postprocess.cpp`；`ui/detection/detection_card.cpp` | 关闭。固定 3 dB 保留冻结 SCN 频率对照；方案说明明确 CNR/频率收边固定 3 dB，卡片噪底裕量供 sample/ROI 时间精修。原型布局、原值与启用矩阵没有为该说明改变；实际窗口 `scnPostprocess` 保存有效固定值。POSTPROCESS 与 FREQUENCY_EVIDENCE 已同步正式逐窗口与辅助 API 的区别。M4 时间精修仍需后续实现并验证，不能在 M3 宣称生效。 |
| P2 | `infrastructure/detection_model_backend.cpp` `Logger` | 关闭。写入与 `lastError()` 读取快照使用同一 mutex，UTF-8 输入在 4096 字节内截断后保存；logger 寿命覆盖所有 runtime/engine/context。最后修改已统一构建并完成 `live-backend-final.log` 六组 head、异步失败与 SCN 对照。未把未触发的并发错误描述为已测量。 |

CUDA 清理判断依据为 NVIDIA 官方 [API synchronization behavior](https://docs.nvidia.com/cuda/archive/12.9.1/cuda-runtime-api/api-sync-behavior.html)：Async API 与 pageable host memory 的同步行为不能作为 buffers 生命周期保证。此处没有将未实际触发的设备错误描述为已测量错误。

## 已核对的边界

生产包与 Fixture 入口分离。模型与 DLL 在 worker 中延迟加载；冻结 SHA、TensorRT 版本、设备、输入/输出形状与 strides 检查均在正式前向前执行。SCN 输入通过十六帧最大谱和每成员 minmax 构造，平均谱没有进入前向；逻辑 B2 顺序调用原固定 B1 引擎，未改变模型形状。17 阶配置明确拒绝未通过的正式输入，不隐式缩小输入或切换后端。

控制块仍为 64 KiB，二进制块仍为 32 MiB。B2/TopK512 通过 `[B,3,1,W]` 原始头传输，完整候选在主进程重建，没有为了控制块大小降低 TopK。每个窗口的原始头按 hash 保存并在引用读取时复核作用域、形状、字节数与 SHA；落盘后才更新窗口完成数和覆盖事实。

终态保留既有 `finishing` 线性化、终态落盘后可见、来源失效同步更新以及失败不冒充成功的规则。最终投影具有显式结束标记；未完整结束的投影回到原始窗口频率观察，仍不虚构活动时间与类别。标记来源发布会原子拒绝冲突 ID，Qt UI 和 QRhi 渲染未被当作 CUDA 推理证据。

新增 SCN 正式逐窗口路径为 CNR 筛选、频带融合、邻带最小噪底向内修正、之后 tracking；未通过 CNR 的候选仍进入未筛选融合占用集合，展示筛选不改变噪底屏蔽。每个窗口保存原始头引用、全部原候选、测量、融合和修正证据；最终 origin 的 `windowPostprocess` 保留它们。生产 SCN 不再调用末尾的整轨迹频率 helper，相关旧文档应同步说明此区别。

最终 UI 尾审已检查 `ui/annotations/annotation_panel.cpp`：没有活动段时，四个几何控件在 `QSignalBlocker` 内清除草稿与 invalid、重置内部值、清空文本、显示破折号占位，且保持禁用；没有发出提交信号或修改 domain 几何。切回有效活动段沿原 `setValue` 路径恢复并更新提示。原生生产场景新增四输入空文本、禁用、无草稿断言；生产运行场景仍经实际检测 Action 展开卡片和运行，未放松旧 23 场景条件。

## 实际执行的自检

- `out/vs2026-qt611-debug_bin/SignalStudioDetectionPostprocessTests.exe`：退出码 0，11 组通过。对应 domain 源码与测试时间早于此二进制，结果可用于当前未再改动的纯后处理层。
- 同目录 `SignalStudioDetectionPipelineTests.exe`：退出码 1，`Current revision does not exist`。二进制时间 00:06:43，早于投影与测试源码 00:13:21；记录为旧二进制结果，不能据此判断修复后的源码通过或失败。主 Agent 应统一重建后重新运行。
- 以上两条旧二进制记录保留为首次审查事实；此次核对修改后 `logs/projection-unit.log`，新的 projection/pipeline 小型自检通过，替代旧 pipeline 结果。
- 已核对修改后 `logs/live-backend.log`：六个 FFSCN B1/B2 synthetic/real/constant 同源 golden 通过，三输出头失败注入及 SCN 固定 B1/逻辑 B2 通过。原头误差遵循预先冻结的绝对加相对容差，不能仅凭最大绝对误差宣称越界。
- 已核对 `diagnostics/pipeline/pipeline-report.json` 与 `logs/live-pipeline.log`：两 profile 正式 Completed、非空对象与 exact recovery 通过；FFSCN 真实取消前缀验证通过。`pipeline-parity.json` 的真实 IQ 逐元素 L1、模型头 L2 通过；SCN L2 是同一冻结 engine 的 Python TensorRT 对照，未冒充 SCN 原 PyTorch 权重对照。
- 已核对 `diagnostics/frequency-parity.json`：全部 eligible 窗口的独立 Python 数学与真实 WAV DSP 频率后处理对照通过；报告明确不含时间事件或历史时频 F1 复现。
- 已阅读上一轮完整 `scripts/build.ps1` Test 的真实日志：19/19 通过，总时长 336.41 秒；这是最后几个修复之前的完整回归，不能替代最后变更后的重跑。最后变更后的 projection 小型自检已通过。
- 已检查截图 helper 的格式参数化：原有场景默认仍严格要求 `torchsig-rfbench-sigmf-v1`，新模型场景显式要求 `detection-v1`；没有放宽来源格式校验、屏幕或 GPU 条件。上一轮 24/25 不算完整验收，已由最后日志替代。
- 最后变更后的 `scripts/build.ps1` Test：19/19 通过，总时长 361.51 秒；对应 `LastTest.log` Qt UI 94 通过、0 失败、0 跳过，249.184 秒。`live-backend-final.log` 六组 FFSCN 原始头、三个异步输出失败 drain/恢复与 SCN 最大谱固定 B1/逻辑 B2 全部通过。
- `native-capture-geometry-final.log` 与对应报告：25/25 个唯一场景通过；连接屏 2、Redmi 27 NU、DISPLAY6、3840×2160、DPR 1.5、2560×1440 全屏。QRhi D3D11 使用 AMD Radeon 880M，真实模型使用 RTX 5060 Laptop GPU，两者职责没有混淆。报告的 `modelInferenceVerified=true`、生产时间事件验证与 A16 为 false，符合 M3 的频率阶段边界；`prototypeComparisonVerified=false` 不冒充自动视觉比较，主 Agent 的目视原型核对另行记录。
- Google C++ 格式空诊断日志已查看；主 Agent 自检记录列出 29 个 C++ 文件、PowerShell AST、Python 静态编译和 M0-P 22 文件指纹不变。此处不把静态检查替代运行验收。
- 未亲自执行此次 live 前向、完整 CTest、当前原生截图或 M5 性能验证；真实日志由主 Agent 执行，此处为独立源码与日志核对。未验证的后续时间精修、二维效果、SCN 实际取消、大规模性能及独立交付范围保留在节点自检记录。

质量审查允许主 Agent 完成 M3 节点证据归档与本地提交，之后才进入 M4；本报告没有提前进入下一节点或把当前结果推送远端。
