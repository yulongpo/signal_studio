# Engine profile matrix

事实来源：本仓库源码与既有包文档。本会话没有反序列化 engine，没有调用 TensorRT。

## 选择规则（本提交）

`selectDetectionExecutionBatch` 只接受 `isDetectionExecutionBatchTier` 的 1、2、4、8、16。

上限是 `requested`、`workerLimit`、`profileMaximum` 的最小值，再取不大于该上限的最大合法档。`profileMaximum == 0` 返回 0。`freeDeviceBytes == 0` 表示显存未知，不按显存缩小。显存已知时，用 `bytesPerUnitBatch * tier + reserveBytes` 的无溢出结果挑最大可容纳档；batch 1 也不够则返回 0。只有 16 同时不大于三个上限且在已知显存下放得下时才返回 16。

`DetectionModelResourceLimits::maximumExecutionBatch` 默认值仍是 2。装载循环把该默认值当作 worker limit，因此默认请求不会选出 16。

## 装载时如何得到 profile 上限

`DetectionModelBackend::Impl::initialize` 在 engine 反序列化之后，对输入名调用 `getProfileShape` 的 min/max。候选形状必须四个维度都落在某个 optimization profile 内。

| 后端 | 探测档 | profile 上限 |
|---|---|---|
| FFSCN | 1、2、4、8、16 | 最后一个能装入 profile 的档 |
| SCN | 只探测 1 | 能装入则为 1，否则拒绝装载 |

SCN 的物理张量 batch 保持 1。用户 batch 大于 1 时仍是多次 `enqueueV3`，不是一次物理 batch。FFSCN 的 `logicalMaximumBatch` 与 `physicalBatch` 都写成选出的物理档，现有 `detection_task` 已按 `logicalMaximumBatch` 分组窗口。

静态包契约（`ffscn_engine_package.cpp`，CONFIRMED_CODE）把非动态 FFSCN 的声明形状夹在 `[1,1,10,65536]` 到 batch 维 2。SCN 声明形状的 batch 维是 1，频率维 32768。动态 v2 契约的 min 是 `[1,1,1,2048]`，max 的 batch 维只要求 `>= 1`。manifest 的 min/max 是包络，不是笛卡尔积都可执行。运行时仍以 engine 的 optimization profile 为准。

## 既有动态包文档（未在本会话复测）

`docs/implementation/wideband-detection/R2/DYNAMIC_MODEL.md` 记录的双 profile 是历史构建结果，不是本次 TensorRT 读回：

| Profile | min | opt | max |
|---|---|---|---|
| 0 | `[1,1,1,2048]` | `[2,1,10,65536]` | `[2,1,64,65536]` |
| 1 | `[1,1,1,2048]` | `[1,1,10,65536]` | `[2,1,16,262144]` |

该文档还写明 workspace 按实际形状的 `updateDeviceMemorySizeForShapes` 分配，不能用最大 profile 激活（文中 6028265472 字节）给小 batch 预分配。本提交的 reserve 使用当前形状的 workspace；若按 profile 夹紧档的 workspace 连 batch 1 都放不下，会改测 batch 1 自己的 workspace，再决定拒绝。

## 未执行

没有打开本机 `detection-models` 里的 engine，没有打印本次进程读到的 min/opt/max。上表的动态 profile 数字不能当作本机当前文件的新测量。
