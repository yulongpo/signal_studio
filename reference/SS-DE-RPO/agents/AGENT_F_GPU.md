# SS-DE-RPO · 子代理 F：CUDA/TensorRT 与显存自适应

你是独立 GPU Backend Owner，目标分支 `rebuild/cursor`；任务文件在 `reference/SS-DE-RPO/`。先读 `04_RESOURCE_GOVERNANCE.md`、`06_VALIDATION_MATRIX.md` 及主代理 M1 契约。

## 独占代码所有权

`infrastructure/detection_model_backend.*` 以及向主代理登记的 GPU runtime/helper。需改 D 的 worker 客户端/引擎调用、C 的 UI 设置时，只发 `interface-requests.md`，不能抢改。

## 必须实施

1. 调查旧限制 `maximumDeviceBytes=4GiB`、`maximumEngineBlobBytes=128MiB`、`maximumExecutionBatch=2` 的实际检查、合理性、模型资产边界、显存总账及运行时 CUDA 设备可用量；区分 CPU model blob 与 device memory。
2. **不允许无依据地把 Batch 2 改为 16。** 首先解析实际 engine optimization profiles 的 min/opt/max 动态 input shape、输出绑定/后处理语义和 context；按有效 profile、空闲 VRAM、engine workspace 和预留量选择 batch。更大 batch 只对真实兼容 engine 在可测条件下启用。
3. CUDA/TensorRT 初始化、context/workspace、device buffers、CPU↔GPU copy、stream synchronize 及 CPU DSP 预取逐项测耗时。模型热复用必须确保参数/设备/文件变更失效并正确回收显存。
4. OOM 自动减小 batch/缓冲区、正确退回 profile 支持范围；无合法可用形状则清楚拒绝，不跳过窗口，不悄悄降低模型输出质量。
5. 在当前 NVIDIA 驱动和真实 engine 上跑可复现 benchmark；仅有 mock 或 CPU 环境时标 `UNVERIFIED_GPU`。不要把历史特定 CUDA/TensorRT 版本当作当前机器已安装的事实。

## 验收与交付

R-03/R-15/R-18，参与首推理性能的实际测量。提交 `engine-profile-matrix.md`、`gpu-resource-inventory.md`、`gpu-perf.md`、`interface-requests.md`、`patch-summary.md`、`agent-status.md`、worktree commit、硬件型号/驱动版本/测试命令与日志路径。
