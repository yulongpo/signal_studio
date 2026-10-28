# GPU performance

没有跑推理，没有跑冷/热启动，没有报吞吐或时延。R-15 与 R-18 的整机结果是 NOT_RUN。

机器上有 NVIDIA 设备，所以这里不把设备标成 `UNVERIFIED_GPU`。没有 engine 装载和计时，推理性能仍然未验证。

已有证据字段在真正推理后会写入 `lastInference` 的 `h2dMs`、`gpuStreamMs`、`d2hMs`、`hostEnqueueMs`、`inputAdaptationMs`。本会话这些字段没有新数值。

## 纯函数检查（不是 GPU 计时）

从 `infrastructure/detection_model_backend.h` 抽出 `selectDetectionExecutionBatch` 的函数体，配上与 `isDetectionExecutionBatchTier` 相同的 1/2/4/8/16 判断，用 VS 2026 x64 `cl /std:c++20` 编成单文件并运行。标准输出 `PASS`，进程退出码 0。这 24 个断言覆盖：profile 0、显存未知不缩小、worker/profile 上限、非档位上限向下取档、默认档 2 不会变成 16、已知显存下 16 不够则取 8、batch 1 不够返回 0、reserve、乘法溢出跳过高档、加法溢出、请求 1、零代价。没有链接 Qt 或 TensorRT，也没有启动工程 configure。
