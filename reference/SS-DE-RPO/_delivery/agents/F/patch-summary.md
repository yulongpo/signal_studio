# Patch summary

默认 `maximumExecutionBatch` 仍是 2。没有把请求或默认档改成 16。

`selectDetectionExecutionBatch` 放在 `infrastructure/detection_model_backend.h`，是纯函数。合法档只用现有的 `isDetectionExecutionBatchTier`。它先取 requested、workerLimit、profileMaximum 的最小上限，再取不大于该上限的最大合法档。profile 上限为 0 时返回 0。空闲显存为 0 时当作未知，不缩小。显存已知时，跳过 `bytesPerUnitBatch * tier` 或再加上 `reserveBytes` 会溢出的档，留下能放进空闲显存的最大档；batch 1 也不够则返回 0。

装载在 engine optimization profile 上限已经算出、并且 `cudaMemGetInfo` 成功之后调用它。每档 device 字节是 input 加三个 head；reserve 是当前形状的 workspace。按较大档的 workspace 连 batch 1 都失败时，先改绑 batch 1 再量一次 workspace。返回 0 则装载失败，错误里有 needed 与 free 字节，不跳窗。选出的档写入既有 evidence 字段 `physicalBatch`。原有 `retryableResourceFailure` 降档仍留给其后的预算检查、`cudaMalloc` 失败和显存校验失败。

源码里原有的 TensorRT `10.11.0.33` 与 compute capability `{12,0}` 记录没有新增硬编码。设备名仍来自 `cudaGetDeviceProperties`。

纯函数单文件检查退出码 0。没有做工程 configure，没有做 TensorRT 推理。
