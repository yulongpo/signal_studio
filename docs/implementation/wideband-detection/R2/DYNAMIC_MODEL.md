# FFSCN 动态输入模型包

本节点使用当前分支的 `tools/detection/model_ffscn.py` 独立网络和冻结的
epoch-1200 权重导出动态 TensorRT 包。没有导入外部训练或检测 GUI 模块。
权重 SHA-256 为
`acd997957dd85b7fc560897874b575d4776e80270d7e377d1de7559e513e40e0`。

## 能力与边界

用户于 2026-10-15 确认 FFSCN 与 SCN 的 `down_nums=11`，本应用采用的模型
输入契约为：频率长度必须是 2 的整数次幂，且不小于 `2^11=2048`。
manifest 对此记录 `input.downNums=11`、`frequencyPowerOfTwo=true`，并保留
`frequencyMultiple=2048` 描述频率金字塔的结构步幅。只有按 2048 对齐但
不是 2 的幂的长度，不符合本次采用的输入契约。

频率适配由应用内部重算 FFT 或插值完成，不将不规则分析长度直接当作用户
检测参数错误。头部沿实际时间维度求平均，没有固定 10 帧的网络要求。
10 帧和 65536 点仍是默认配置及复现条件。短输入使用实际谱帧，工具没有
复制谱帧来凑足默认数量。平均、最大、最小、瞬时谱的构建属于应用参数；
原始张量探针不代表四类谱各自的检测效果已经验证。

部署包为 `signalstudio-detection-model-package-v2`，保留
`ffscn-gui-iq-v1` / profile version 1，新增 `adaptationVersion=2`。
输入为 contiguous NCHW float32 `spectrum`，输出为原始 `hm`、`bw`、`off`，
形状 `[B,1,1,F/4]`，输出步幅为 4。未改变权重或解码门限。

最终引擎采用两个优化配置：

| 配置 | 最小 `[B,C,T,F]` | 优选 | 最大 |
|---|---|---|---|
| 0，较长时间维 | `[1,1,1,2048]` | `[2,1,10,65536]` | `[2,1,64,65536]` |
| 1，较宽频率维 | `[1,1,1,2048]` | `[1,1,10,65536]` | `[2,1,16,262144]` |

manifest 的 `input.minShape/maxShape` 是包络，**不表示整个包络的笛卡尔积均
可执行**；顶层 `profiles` 数组及引擎实际 profile 才是资源范围。后端应按
实际形状选择可覆盖的配置。两种配置均不覆盖的输入需要明确报告当前部署
资源或包能力，不能描述为算法要求固定帧数，也不能静默裁掉真实观测。

执行上下文采用 `USER_MANAGED`，选择 profile、设置实际形状之后调用
`update_device_memory_size_for_shapes()`，按实际激活大小分配。不能用最大
profile 的激活大小为每一个小任务预分配显存。

FP32 构建关闭 FP16 和 TF32，workspace 限制为 1024 MiB；运行库为 TensorRT
10.11.0.33 / CUDA runtime 12090，目标设备为 NVIDIA GeForce RTX 5060 Laptop
GPU，compute capability 12.0，无跨 GPU 或跨 TensorRT 版本兼容声明。

## 代表性探针与失败记录

固定随机种子 20261015，探针比较原始头张量，不经过阈值、NMS 或标签评价。
容差保持 `atol=1e-4, rtol=1e-3`，逐元素判定为
`abs(actual-reference) <= atol + rtol*abs(reference)`。

探针包含单帧/2048 点、三帧/8192 点、默认批量 2 的 10 帧/65536 点、
32 帧/2048 点及单帧/131072 点；后两项分别覆盖较长时间维及第二个宽频配置。
每项均保存输入和 PyTorch 原始头的 little-endian float32 文件，供当前分支
C++ 后端使用相同输入直接对照。最终文件身份、结果和资源记录见
`dynamic-model-probe.json`。

双配置引擎首次构建耗时 462.669 秒，五项探针全部通过；随后复用同一引擎
重新执行五项探针仍全部通过，三个头均无超容差元素。前四项选择配置 0，
131072 点项选择配置 1。实际激活字节依次为 4154368、19306496、932087808、
48203776、103635456；引擎最大 profile 激活为 6028265472 字节，不能拿该
最大值为小任务预分配。

最终引擎 `ffscn_epoch1200_dynamic_fp32.engine` 为 23722236 字节，SHA-256 为
`4e14e422e7966eb6f554e322c2d390534650e1b8dd0cee6e04f2ed4305cefc03`；
manifest SHA-256 为
`776ebd3261c063567e443f5b96d5c93a8e50449d9d491bb1242f8c7bf2575786`。
该 manifest 在用户确认输入契约后补充下采样层数和2次幂要求；复用原引擎
重新检查现有五项探针仍全部通过，没有重建引擎。

首次尝试单 profile 最大 `[2,1,64,262144]`，TensorRT 在 88.331 秒后未能
生成引擎。日志显示候选实现请求约 7.5–15 GiB 显存，本机物理显存为
8151 MiB。失败 JSON 和日志保留在 R2 `diagnostics` / `logs`，没有作为通过
证据。随后单独的 `[2,1,16,262144]` 配置构建耗时 302.618 秒，三项原始
探针通过；其引擎、manifest 和 golden 已保存在 `dynamic-ffscn/single-profile-b/`。
该单配置只作构建诊断，不作为最终双配置交付包。

这里验证的是结构可执行性、有限值、动态形状及 PyTorch/TensorRT 原张量
一致性。它不是模型效果评估，不证明不同 FFT 的检出率相同，不替代真实
IQ 流程、标签精度或实时吞吐验收。记录的一次同步前向耗时也不是冷/热
性能基准。Torch peak allocated 仅为 PyTorch 跟踪的分配，不能当作进程总
GPU 占用。

工具静态检查通过 Ruff、Python 语法及 80 字符行宽核对。隔离 staging
自检六项通过：正常复制后身份一致，SCN 与运行库哨兵不变，失败探针、
变更 manifest、变更 engine 及绝对资产路径均拒绝。M3 原引擎再次验证为
`d2de250b11d6bc4a3ccec6d95a232b03ba011e085698d525c3454387094bfe81`，
保持原字节。这些检查不替代应用原生后端和检测旅程验收。

## 复现与部署

新资产位于 `out/tasks/wideband-detection/R2/assets/dynamic-ffscn/`，历史 M3
模型包保持原字节。完整导出、构建和探针命令：

```powershell
& D:/profiles/anaconda/python.exe tools/detection/model_dynamic_package.py `
  --checkpoint D:/project/FFSCN_train2/FFSCN_TEST_GUI/config/epoch_1200.pth `
  --trt-root out/tasks/wideband-detection/M3/assets/runtime/tensorrt-10.11 `
  --output out/tasks/wideband-detection/R2/assets/dynamic-ffscn `
  --report out/tasks/wideband-detection/R2/diagnostics/dynamic-model-probe.json `
  --output-bin out/tasks/wideband-detection/R2/local-builds/dynamic-package
```

加入 `--reuse-engine` 可核对 checkpoint、图源码、ONNX、引擎、profile 和精度
身份后复用已构建引擎，重新执行探针。加入 `--stage-only --output-bin <bin>`
仅复制已通过探针且 SHA-256 相符的 FFSCN engine 和 manifest 到
`<bin>/detection-models/ffscn-gui-iq-v1/`。不复制 Python、训练目录、SCN
模型或 Qt/CUDA/TensorRT DLL，也不改动既有部署依赖。

`--stage-only` 仍需提供完整参数；checkpoint 和 runtime 参数只为保持同一
CLI 形状，该模式读取已有探针报告验证 manifest 身份，不重新构建或运行
GPU 探针。完整命令如下：

```powershell
& D:/profiles/anaconda/python.exe tools/detection/model_dynamic_package.py `
  --checkpoint D:/project/FFSCN_train2/FFSCN_TEST_GUI/config/epoch_1200.pth `
  --trt-root out/tasks/wideband-detection/M3/assets/runtime/tensorrt-10.11 `
  --output out/tasks/wideband-detection/R2/assets/dynamic-ffscn `
  --report out/tasks/wideband-detection/R2/diagnostics/dynamic-model-probe.json `
  --stage-only `
  --output-bin out/tasks/wideband-detection/R2/local-builds/dynamic-package
```

golden 命名为 `shape-b<B>-t<T>-f<F>.input.f32` 及
`.hm.reference.f32`、`.bw.reference.f32`、`.off.reference.f32`，具体形状、
字节数和 SHA-256 列于报告。ONNX、engine、timing cache、原始张量及临时日志
保持在被忽略的任务目录；本目录只归档说明和小型身份/探针报告。
