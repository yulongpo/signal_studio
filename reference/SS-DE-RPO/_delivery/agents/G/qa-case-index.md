# SS-DE-RPO · Agent G 用例索引

基线：`82935ab34ec6126707da152bf46d1f352458ab0c`（`fix/ss-de-rpo-g`）。  
本索引只分类 R-01～R-20。本提交实际落地的可执行用例是 R-01、R-04、R-05、R-12（`tests/ss_de_rpo_logic_tests.cpp`）和 R-08 的编译期 SKIP（`tests/ss_de_rpo_ui_tests.cpp`）。其余条目没有在本提交里新增断言。

证据层级：`unit` 纯逻辑；`qt` 真实界面线程；`worker` 合法 IQ 与 worker；`gpu` 真实 NVIDIA/TensorRT。fixture/mock 不得标成 GPU PASS。

| ID | 层级 | 环境依赖 | 本提交落点 | 原始证据 |
|---|---|---|---|---|
| R-01 | unit | `SignalStudioInfrastructure`，无 NVIDIA。Sample `makePlan`。 | `tests/ss_de_rpo_logic_tests.cpp` `R-01`、`R-01-cell-ceiling-allowed`、`R-01-cell-ceiling-plus-one` | 未运行。源码期望见 `red-green-results.md`。 |
| R-02 | unit | 自定义 1/2/4/8/16 GiB 授权下发；超设备资源需真实 RAM 探针。 | 未写 | 无 |
| R-03 | unit | checked arithmetic、低 RAM 拒绝与释放。 | 未写 | 无 |
| R-04 | unit | `applyDetectionEvidencePreflight`。2044 窗、每窗 196608 B。不需要真实卷。 | `R-04`（retain off、配额 0）；`R-04-explicit-256MiB`（显式 256 MiB 仍拒绝） | 未运行 |
| R-05 | unit | 同上，外加 `setDetectionVolumeFreeBytesProbeForTest` 短剩余。需求 401866752 B。 | `R-05` | 未运行 |
| R-06 | unit + worker | Sample 20/363/2044 窗。20/363 可用合成；2044 需要真实或等价长 IQ，不能用 mock 活动段充数。 | 未写 | 无 |
| R-07 | unit | Sample/ROI/Coarse 活动段语义。已有 `tests/detection_time_pipeline_tests.cpp` 不在本提交改动。 | 未写 | 无 |
| R-08 | qt | `DetectionCard::startLatchActive`、按钮「准备检测中」、无未完成 run。需要已保存工程和真实 Complex IQ，否则 `submit` 在校验处返回、不会发 `requestSubmitted`。 | `tests/ss_de_rpo_ui_tests.cpp` 打印 SKIP，返回 0。`BLOCKED_UNTIL_AGENT_C` | 未做点击。基线头文件没有 `startLatchActive` |
| R-09 | qt + worker | 预检/worker 初始化期间取消、关闭。需要可启动 worker 与界面线程。 | 未写 | 无 |
| R-10 | worker | 损坏 engine/source、磁盘失败。必须复制或模拟输入，不能改用户 IQ。 | 未写 | 无 |
| R-11 | worker | 159/159、363/363、2044/2044。推理结束不等于 Completed。需要真实模型与 IQ。 | 未写 | 无 |
| R-12 | unit | `preserveDurableModelEvidence` / `detectionModelEvidenceIsDurable`。假 `lastInference` JSON，不落盘。 | `R-12-nondurable`、`R-12-raw-head-budget`、`R-12-legacy-combined` | 未运行 |
| R-13 | qt | UX-R6 同通道结果：改参数/模型不清除已提交结果。需要工程重开。 | 未写 | 无 |
| R-14 | worker | final shard 损坏、工程提交失败、恢复。需要可写临时工程，不碰用户工程。 | 未写 | 无 |
| R-15 | gpu | TensorRT profile 与 batch 1/2/合法更大档。无 NVIDIA 时只能标 `UNVERIFIED_GPU`。本机本提交未跑。 | 未写 | 无。`UNVERIFIED_GPU` |
| R-16 | unit | 高密度轨迹、比较上限。已有 `tests/detection_postprocess_tests.cpp` 不在本提交改动。 | 未写 | 无 |
| R-17 | worker | 高持续时间、高 sampleCount。有界 RAM/IO。不能伪造活动段。 | 未写 | 无 |
| R-18 | worker + gpu | 模型/数据/参数变化后的缓存失效。GPU 部分 `UNVERIFIED_GPU`。 | 未写 | 无 |
| R-19 | qt | 界面响应。目标 p95 < 100 ms 只作报告，不能当硬通过。 | UI SKIP 未启动 `QElapsedTimer` | 无实测毫秒 |
| R-20 | qt | 标记、工程重开、窄带、PSD、时频/瀑布。需要真实界面，不能用单张截图代替。 | 未写 | 无 |

故障注入只登记、本提交不执行：低可用 RAM/VRAM/磁盘、损坏 engine、损坏 shard、工程原子提交失败、worker crash、缓存来源变更、超长信号。文件输入必须先复制。没有用户 IQ 被读取或修改。
