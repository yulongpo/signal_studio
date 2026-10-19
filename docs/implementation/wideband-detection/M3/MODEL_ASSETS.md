# M3 模型资产与部署数值核对

2026-10-13 资产预备记录。原始 JSON 报告保留当时路径和结论，迁移后不会重写历史原始报告。本节点正式 worker、真实任务、频率层对照及原生验收以 `IMPLEMENTATION.md`、`SELF_REVIEW.md` 和新归档证据为准；完整时间事件与二维评价仍由 M4 完成。

## 已验证结果

主线 `ffscn-gui-iq-v1` 的 epoch-1200 已由本仓库独立实现并导出。478 个状态张量严格匹配，参数量为 2,695,992；同时核对旧 checkpoint 中保存的卷积 stride、padding、group、BatchNorm eps 和 activation 等不属于 state dict 的属性。没有导入外部训练或 GUI 的业务模块。

checkpoint 是完整的 DataParallel pickle。本仓库保持 `weights_only=True`，只允许已列出的 PyTorch 标准模块，并把旧自定义类映射为没有 `forward` 的惰性容器，以提取状态与算子属性。先验证冻结 SHA-256，再读取权重；未知全局类和缺少/多余状态均拒绝。

| 对照 | 实际检查 | 结果 |
|---|---|---|
| PyTorch CUDA → TensorRT FP32 | 合成谱、真实 GUI WAV 前缀、全零模型输入，各 B=1/B=2，共六组；hm/bw/off 原张量、解码数量、次序及边界 | 六组全部通过，正式默认精度为 FP32 |
| PyTorch CUDA → TensorRT FP16 | 与 FP32 相同输入和门槛 | 六组均有原张量超差；真实候选次序也变化，保持实验状态 |
| PyTorch CUDA → ONNX Runtime CPU | 相同六组输入 | 合成两组通过；真实/常量存在少量 bw/off 超差，不能作为等价默认回退 |
| 原生 C++ → PyTorch CUDA golden | 从独立目录运行，PATH 仅保留 Windows System32；B1/B2 六组原张量读回 | 六组全部通过 |
| SCN 控制 | 16 个真实谱帧的最大谱，三个 32768 点频率窗，平均谱推理调用数为 0 | 三次真实前向均有限；不作 IQ/仪器域精度声明 |
| FFSCN-17 实验 | 真实谱的五个宽度，冻结 ONNX CPU → 原 FP32 engine | 8192 通过；16384/32768/65536/131072 原张量超差，保留实验 |

原张量门槛在运行前固定为 `atol=1e-4, rtol=1e-3`。解码候选数量一致，按分数及稳定索引排序后边界差不超过 0.05 bin、分数差不超过 0.001。主线最终六组 FP32 的最大有序边界差为约 0.000978 bin。本检查没有使用标签计算准确率，也没有修改人工标签补偿推理差异。

真实输入来自 M0 冻结的 `gui-local10-m0-20261013` 首文件 `20260728-044906-196...wav`，SHA-256 为 `1757d92722e78776a6877b4fe5f29734fd2975989aaef1d52f8103aaa310b18b`。只读取其前 400,000 个复数样本，即 20 Msps 的 20 ms 前缀，用来生成两个 10 帧模型上下文；此处候选 11/12 是窗口频带候选数量，不是时频事件数量。

## 资产与环境

| 资产 | 包内位置 | SHA-256 |
|---|---|---|
| 主 checkpoint | 导出时显式传入，先验证冻结身份 | `acd997957dd85b7fc560897874b575d4776e80270d7e377d1de7559e513e40e0` |
| ONNX opset 18 | `out/tasks/wideband-detection/M3/assets/epoch1200/ffscn_epoch1200.onnx` | `40d666f5de70b6e119a93f23be234502dd6263136af6ff7a505904cbb43d2fb4` |
| FP32 engine | `out/tasks/wideband-detection/M3/assets/epoch1200/ffscn_epoch1200_fp32.engine` | `d2de250b11d6bc4a3ccec6d95a232b03ba011e085698d525c3454387094bfe81` |
| FP16 候选 | `out/tasks/wideband-detection/M3/assets/epoch1200/ffscn_epoch1200_fp16.engine` | `8b27e3e484e9d7f9c8aa456d4a95d9f066a613c471bd2531f312bdcb50572899` |

真实运行环境为 Windows 11 x64、RTX 5060 Laptop / sm120、驱动 591.84、PyTorch `2.12.0+cu132`、TensorRT `10.11.0.33`、CUDA runtime `12.9.79`。engine 在本机 GPU 构建，FP32 关闭 TF32；没有开启 hardware/version compatibility。不能默认迁移至其它 GPU 或 TensorRT 版本。

主 engine 输入为 `spectrum:float32[B,1,10,65536]`，动态 B 的 min/opt/max 为 1/2/2。输出 `hm/bw/off:float32[B,1,1,16384]`，三路均为 stride-4 频率输出。所有 IO 的实际格式为 `kLINEAR`，行优先连续；输入 strides 为 `[655360,655360,65536,1]`，输出为 `[16384,16384,16384,1]`。模型不输出真实调制类别或时间起止。

SDK 资源全部位于 `out/tasks/wideband-detection/M3/assets/runtime/tensorrt-10.11`。官方 pip 包提供 builder、parser、plugin 和 runtime DLL，头文件来自 NVIDIA TensorRT 固定 commit `9255eb39e6642787828a4c1f7fc1d09fe004e7a2`；MSVC import library 根据官方 DLL 的导出表生成。CUDA runtime wheel 缺少 `crt` 头文件，已从匹配的官方 `nvidia-cuda-nvcc-cu12==12.9.86` 补齐，不使用外部应用的 SDK 或源码目录。安装路径的能力边界见 [NVIDIA pip 安装说明](https://docs.nvidia.com/deeplearning/tensorrt/latest/installing-tensorrt/install-pip.html)。

原生探针只需输出目录内 `ModelNativeProbe.exe`、`nvinfer_10.dll`、`cudart64_12.dll` 及本项目模型/输入文件。六次读回已经在清理 PATH 后运行，不依赖 Python、Qt 安装目录或外部源码目录。正式 Qt 产品的部署仍需自己的验收。

## 数值版本和验证边界

周期 Hann、1 ms hop、完整矩阵 ddof=1 标准差、全有效带宽以及真实 10 帧上下文保持冻结语义。FFT 支持在 20 Msps 下为 65536 样本，远大于 20000 样本步长；边缘零支持必须记录，不把帧步长当成 FFT 记录时长。

模型输入工具的数值版本为 `reference-power-floor-double-log-v1`：保留参考的 power-floor 顺序，density 除法和 log 使用 double，最终转 float32，避免参考全零输入在 float32 除法时下溢产生 `-inf`。该边界修正与“直接给模型全零张量”的探针分开。M0 冻结 profile 原文件及其 hash 未修改；完整预处理的 CPU/GPU及 native source provider 对照仍由 M2/M3 整合记录。

本节报告中的单次 forward 延迟不是持续吞吐测量。六组张量通过也不证明 10 文件 TF-F1、短突发召回、ROI 精修、长文件取消、Qt 联动或独立产品包通过。FP16、FFSCN-17 和 ONNX CPU 的超差没有通过放宽门槛消除。

## 重复执行

以下命令使用显式的开发 Python；模型、SDK 和运行日志写入被忽略的 `out/`，报告写入 `M3/`。权重路径是用户指定的冻结来源，不是构建输入中的外部业务代码。

```powershell
& D:/profiles/anaconda/python.exe -m pip install --target out/tasks/wideband-detection/M3/assets/runtime/tensorrt-10.11 'tensorrt-cu12==10.11.0.33'
& D:/profiles/anaconda/python.exe -m pip install --target out/tasks/wideband-detection/M3/assets/runtime/cuda-nvcc-12.9 'nvidia-cuda-nvcc-cu12==12.9.86'
& D:/profiles/anaconda/python.exe tools/detection/model_stage_runtime.py --root out/tasks/wideband-detection/M3/assets/runtime/tensorrt-10.11 --msvc-bin 'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64' --report docs/implementation/wideband-detection/M3/runtime-manifest.json
& D:/profiles/anaconda/python.exe tools/detection/model_probe.py --checkpoint D:/project/FFSCN_train2/FFSCN_TEST_GUI/config/epoch_1200.pth --wav D:/project/FFSCN_train2/FFSCN_TEST_GUI/test_data/data/20260728-044906-196_bw_20000000_sampleTime_0.8_rollOff_0.35.wav --trt-root out/tasks/wideband-detection/M3/assets/runtime/tensorrt-10.11 --report docs/implementation/wideband-detection/M3/model-parity.json
./tools/detection/model_build_native_probe.ps1
```

已有两个 engine 时，`model_probe.py --reuse-engines` 只重新执行数值检查。输入 golden 和部署 manifest 也在 `out/tasks/wideband-detection/M3/assets/epoch1200/`。原生探针用法为 `ModelNativeProbe asset-directory prefix batch report`，例如 `../epoch1200 real-wav-b2 2 native.json`。

SCN 与 FFSCN-17 另用 `model_controls.py` 显式指定冻结资产目录，工具验证 M0 hash 后复制模型资产至 `out/tasks/wideband-detection/M3/assets/controls/`，没有链接或运行外部业务实现。

## 证据文件

- `model-parity.json`：各输入、B1/B2、三个运行时的原张量误差、候选误差及实际环境。
- `runtime-manifest.json`：SDK/CUDA 头文件、import libraries、DLL 的相对路径、大小与 SHA-256。
- `native-*-b*.json`：六组原生 C++ GPU 读回结果。
- `control-model-probes.json`：SCN 最大谱调用及 FFSCN-17 五宽度的逐张量结果。
- `ffscn_epoch1200.manifest.json`：部署模型 IO、环境、资产身份与精度选择。
