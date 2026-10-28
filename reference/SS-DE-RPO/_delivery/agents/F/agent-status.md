# SS-DE-RPO · 子代理交接

- **Agent**：F
- **任务编号**：SS-DE-RPO GPU batch，契约第 6 节
- **审查基线及实际执行 HEAD**：基线 `82935ab34ec6126707da152bf46d1f352458ab0c`。本文件在该次提交内，提交 SHA 以 `git rev-parse HEAD` 为准。
- **Worktree 路径**：`D:\project\ss-de-rpo-wt\f`
- **工作分支和 commit**：`fix/ss-de-rpo-f`，消息 `fix(ss-de-rpo): select tensorrt batch inside profile and vram`
- **文件所有权/实际变更文件**：`infrastructure/detection_model_backend.h`、`infrastructure/detection_model_backend.cpp`、`reference/SS-DE-RPO/_delivery/agents/F/` 下的交付说明。未改 worker client、UI、CMakeLists。
- **与主代理冻结接口的兼容情况**：兼容。使用契约中的函数签名和既有 `physicalBatch` / `logicalMaximumBatch`。默认 batch 仍为 2。

## 1. 已确认根因（带证据分类）

| 问题 | 事实类型 | 路径/行号或日志 | 影响 | 修复 |
|---|---|---|---|---|
| 装载已按 profile 夹紧物理 batch，但夹紧后没有再用空闲显存选档 | CONFIRMED_CODE | `infrastructure/detection_model_backend.cpp` 的 profile 循环与 `cudaMemGetInfo` | 显存不够时要等到分配或 4 GiB 预算失败才降档 | profile 已知且 `cudaMemGetInfo` 成功后调用 `selectDetectionExecutionBatch` |
| 默认 batch 被要求保持 2 | CONFIRMED_CODE | `DetectionModelResourceLimits::maximumExecutionBatch = 2` | 不能把默认或普通请求抬到 16 | 默认值未改；16 只在三个上限都允许且显存放得下时出现 |

## 2. 代码变更摘要

见 `patch-summary.md`。

## 3. 验证

| 用例 ID | 运行环境与命令 | 测试结果 | 日志/证据位置 |
|---|---|---|---|
| 纯函数 24 个断言 | VS 2026 x64，`cl /std:c++20`，函数体来自本提交的头文件 | PASS，退出码 0 | 本会话标准输出 `PASS`。检查源文件未入库 |
| R-15 / 整机推理 | 未启动 | NOT_RUN | `gpu-perf.md` |
| 工程 CTest / configure | 未启动 | NOT_RUN | 按任务要求没有做完整 configure |

## 4. 关键性能与资源峰值

没有新的推理计时。GPU 采样见 `gpu-resource-inventory.md`：RTX 5060 Laptop，驱动 591.84，总显存 8151 MiB。free 随桌面占用变化，不作预算。

## 5. 向其他代理请求的接口及其处理情况

见 `interface-requests.md`。没有跨所有权改动。

## 6. 未完成 / 风险 / 下一步

- `detection_model_backend.cpp` 的 TensorRT 装载路径没有经过本仓库 configure/编译。
- 没有读到本机 engine 的实时 min/opt/max。
- G 仍需把 `selectDetectionExecutionBatch` 登记进正式测试目标。
- `freeDeviceBytes == 0` 按契约表示未知。CUDA 若成功但报出 0 空闲字节，选择函数不缩小，后续显存校验和原降档仍然生效。

## 7. 交接声明

已在自己的 worktree 提交，未修改其它 owner 文件；未用 mock 当作真实硬件；没有修改目标分支以外的正式分支。未推送。
