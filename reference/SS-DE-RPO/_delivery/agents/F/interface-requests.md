# Interface requests

没有要求修改 worker client、UI 或 `CMakeLists.txt`。

现有交接已经够用：

- `DetectionModelBackend::evidence()` 的 `physicalBatch` 写入选出的物理档。FFSCN 同时把 `logicalMaximumBatch` 设成同一档。
- `infrastructure/detection_task.cpp` 已用 worker evidence 的 `logicalMaximumBatch` 收紧 `executionBatchSize`，并复制 `physicalBatch`。窗口仍按这个档分组，不会因为档变小被丢掉。
- SCN 的 `physicalBatch` 保持 1；`logicalMaximumBatch` 仍是多次 `enqueueV3` 的次数。选择函数在 SCN profile 上限为 1 时不会把它抬高。

请 G 在自己的测试里直接调用 `signalstudio::selectDetectionExecutionBatch`。函数在 `infrastructure/detection_model_backend.h`，不包含 CUDA。本代理没有登记 CMake 目标。

选择函数返回 0 时，装载失败，错误文本包含 `needed` 字节和 `free` 字节，并清掉 `retryableResourceFailure`，避免外层再把已经无法容纳的 batch 1 降档重试。这不是跳窗。
