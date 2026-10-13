# M0 模型、数据与运行环境资产审计

核对日期：2026-10-13（Asia/Shanghai）。机器可供开发验证使用；主线 epoch-1200 权重和两类格式样例均存在，原生模型发布仍需完成转换和数值验证。本节点没有加载权重、运行模型、导出 ONNX、构建 engine、训练或测量性能。

完整机器可读记录及逐文件 SHA-256 见 [assets.json](assets.json)。`verified` 只表示本次核对了对应身份或元数据；`available` 表示明确用途所需的文件或接口存在；`needs_conversion` 表示源资产已具备但目标表示/验证未完成；`unavailable` 表示指定审计路径或环境中不存在。任何状态均不能代替模型效果或部署验收。

## 主线权重和参考代码

`D:/project/FFSCN_train2/FFSCN_TEST_GUI/config/epoch_1200.pth` 为 11,145,701 字节，当前 SHA-256 是 `acd997957dd85b7fc560897874b575d4776e80270d7e377d1de7559e513e40e0`，与指定身份一致。文件只做顺序哈希读取，没有反序列化。

只解析 `business/config.py` 的字面量 `DEFAULTS`，没有导入参考模块。默认检测配置为 65536 bins、1 ms STFT 帧步长、10 帧模型窗口、5 帧窗口滑动、batch 2、top-k 512、score 0.7、NMS IoU 0.3；同目录 legacy INI 也保留这些值。1 ms 描述相邻谱帧步长，不能当成 FFT 窗长。

参考 FFSCN 仓库位于 `D:/project/FFSCN_train2`，分支 `feat/ffscn-gui-optimization`，HEAD 是 `d2d9dd947c58314d09406cf9fd0bfbe89a57c465`。当前工作树有修改和未跟踪文件，其中 `business/evaluation.py` 未跟踪。清单固定了 24 个 GUI 配置、模型架构、预处理、后处理、文件读入、评价器和测试源码文件的当前哈希、HEAD blob 与逐文件 Git 状态；规划清单中已有身份的文件均匹配。未复制外部业务源码。

`contracts/wideband-detection/profiles/ffscn-gui-iq-v1.json` 的模型架构字段与可读参考来源一致：GUI `ui/main_window.py:2041` 实际传入 downNums=11，CLI 默认也是 11；`business/detection.py:93` 构建 `CarrierDetectionNet`，单样本 `(1,10,input_length)`、`mobilenet_v3_large`、`reduced_tail=True`、单层 FPN/64 channels；`:347` 将窗口矩阵 stack 后 `unsqueeze(1)`，在默认 NFFT 下得到 `[B,1,10,65536]`。`algorithm/network.py:174` 返回 `hm/bw/off` 字典，默认 `is_training=True` 保留原始头输出。此项是静态来源核对，实际模型张量 shape 和数值尚未验证；profile 文件身份与来源行号已记录。

epoch-1200 的 `config/` 中没有 ONNX 或 engine。此项状态为 `needs_conversion`，其他 FFSCN 权重或 FFSCN-17 engine 不可替代其身份。清单中的 ONNX/engine 缺失结论限于实际审计路径，不声称穷尽本机所有磁盘。

## 本机环境

| 资产 | 当前证据与状态 | 验证边界 |
|---|---|---|
| CPU / OS | AMD Ryzen AI 9 H 365，10 核 20 线程；Windows 11 x64，10.0.26300，`verified` | WMI 元数据 |
| NVIDIA | RTX 5060 Laptop GPU，8151 MiB，driver 591.84，compute capability 12.0，`verified` | `nvidia-smi` 设备查询，无负载或吞吐测试 |
| Qt | `D:/softwares/Qt/6.11.1/msvc2022_64`，6.11.1，Core/Gui/Widgets/qsb/windeployqt 存在，`verified` | qmake 和 DLL/EXE 版本，无本节点应用启动或 QRhi 验证 |
| MSVC / CMake | Visual Studio 18.10.12224.181；x64 compiler 19.51.36260.0；CMake 4.3.1，`verified` | vswhere、版本查询及已有 CMake cache，无编译 |
| CUDA Toolkit | 本机工具链目录 v12.4，nvcc 12.4.131，`verified` | 不等于 PyTorch wheel 或 TensorRT bundle 的 CUDA 版本；未编译 CUDA 源码 |
| PyTorch | 默认 Anaconda 与 `ffscn_torch` 都能导入 2.12.0+cu132、报告 CUDA available，并识别同一 RTX 5060；wheel 支持列表含 `sm_120`，`verified` | 只做库导入、设备/架构查询，无张量计算或模型前向 |
| ONNX | 默认 Anaconda 与 `ffscn_torch` 可导入 1.22.0，`available` | 未导出、检查或执行模型图 |
| ONNX Runtime / TensorRT Python | 三个查询环境均没有安装，`unavailable` | 包元数据查询 |
| TensorRT native runtime | SCN 参考目录中 headers 为 10.11.0.33；`nvinfer_10.dll` 可载入，`getInferLibVersion()` 返回 101100，`available` | 只查询运行库版本，没有反序列化 engine |
| TensorRT conversion tools | PATH 和被审计 SDK `bin/` 无 `trtexec.exe`，该目录无 `nvonnxparser_10.dll`、`nvinfer_plugin_10.dll`，`unavailable` | parser 头文件和 import lib 存在，不能据此认定完整转换链可用 |

Python 解释器分别为 `D:/profiles/anaconda/python.exe`（3.13.12）、`D:/profiles/anaconda/envs/ffscn_torch/python.exe`（3.13.14）和 `D:/coding/FFSCN_train2/env/python.exe`（3.13.14）。第三个环境只查询解释器与包元数据，没有导入库或查询设备。默认环境有 `onnxscript` 0.7.1 的安装元数据；两个专用环境均缺少该包，未验证默认环境的 exporter。

TensorRT 库在 `D:/project/SCN_based_PSD_Detection/third_party/tensorrt`，其中 cudart 元数据对应 12.9，独立于安装的 CUDA Toolkit 12.4 与 PyTorch wheel CUDA 13.2。此次只是依赖资产核对；它尚未成为本仓库已配置的构建输入。不能把参考仓库中的运行库可载入描述为 Signal Studio 原生推理或发布包通过。

## 数据身份与缺失情况

GUI 的 `test_data/data` 当前有 10 个 WAV，`test_data/label` 有逐个同名的 10 个 TXT 和 10 个 JPG。样本名称、WAV 内容哈希和 TXT 内容哈希分别唯一。每个 WAV 都是 PCM、双通道、16 bit、20 Msps、16,000,000 个复样本、0.8 s，文件大小均为 64,000,044 字节。TXT 文本有 117 条载波行、594 个 `TIME` 数值对；这些是字面量盘点，不是调用参考标签解析器得到的评价结果。每个 WAV/TXT/JPG 的路径、大小、哈希和 WAV 头已经写入清单。

这组数据命名为新的冻结基线 `gui-local10-m0-20261013`。在当前生成器输出枚举中没有找到这些文件的精确同名原始 WAV/TXT；历史临时 `D:/project/FFSCN_train2/.codex_ffscn_verify_20260830/selftest.json` 也不存在。因此 10 文件和 594 段计数相符，仍不能证明已经恢复历史评价所用的文件哈希与预测结果。历史 TF-IoU 指标应继续单列，后续用本清单和当前冻结评价器形成新结果。

第一类格式在用户指定生成器根目录选定一对实际 golden：

- `D:/coding/FFSCN_train2/broadband_signal_generation/test_sig2/data/20260728-025821-162_bw_20000000_sampleTime_0.8_rollOff_0.35.wav`
- `D:/coding/FFSCN_train2/broadband_signal_generation/test_sig2/label/20260728-025821-162_bw_20000000_sampleTime_0.8_rollOff_0.35.txt`

该 WAV 同样是双通道 PCM16、20 Msps、0.8 s。已核对这对文件及 `write_test_record.m`、`base_function/save_signal_wav.m` 的身份；writer 的 I 后 Q 交替写入及 TXT 多时间段格式仅作为只读语义证据。没有重新生成数据或检查整个生成器数据集。

第二类是 `W:/wbSig53_rfbench20m/train/baseline/0000000.{json,sigmf-meta,sigmf-data}`。三文件当前哈希均与规划核验记录一致：`cf32_le`、20 Msps、400,000 个复样本、3,200,000 字节、20 ms、RF 中心 2.5 GHz，JSON 的 4 个 events 与 SigMF 的 4 个 annotations 在样本起止、类别和频率换算后一致。此文件按 `rfbench-v1` 样例冻结，名义带宽来自生成参数；不能把单文件检查扩大为通用 TorchSig 格式支持或实测占用带宽。

## 独立补充资产

| 资产 | 当前可用性 | 本期用途与限制 |
|---|---|---|
| HY epoch-210 频率基线 | 本地 `weights/model_epoch_0210.pth` 与 delivery 副本 SHA-256 均为 `b6cd6aaf28e695b4bcae94db25dd527d2b717269b1562cf47e0840d09d8a253a`；报告、summary、1000 项 manifest 与源码快照存在，`verified` | 频率历史基线；manifest 数据位于服务器 `/fast_data/HY_60MHz_merged`，本次未挂载/重哈希对应 1000 对数据，回放数据 `unavailable`，未复跑指标 |
| SCN 最大谱对照 | `models/scn_model.engine` 与 DetectionEngine/BandwidthRefiner/DetectionConfig 当前文件已哈希，`available` | 参考分支 `scn_dev`、HEAD `be95399aa17409a45380bdf46f3e42e75bfc647b` 也有修改与未跟踪文件；未验证最大谱数值或运行 engine |
| FFSCN-17 实验 | 本地 ONNX、FP32 deployment、FP32 copy、FP16 candidate 哈希全部与提交 `649cc247f948d8ef71975c9ae1ed4675e12bb68a` 的 manifest 一致，`available` | manifest 当前工作树文件缺失，已核对可读取的 Git blob；只作为独立实验，远程 checkpoint 未探测，FP16 未提升为正式配置，未运行模型 |

这些资产分别保留自己的模型、输入和评价语义，不扩展主线范围。FFSCN-17 的 manifest 是历史转换记录；本次文件哈希匹配不能代替当前 runtime 的反序列化、前向或精度验证。

## 后续可执行事项

1. 在本仓库独立建立 epoch-1200 架构与导出实现，先固定输入/输出和 CPU/PyTorch 对照张量，再导出 ONNX 并保存 checkpoint、架构、ONNX 的哈希及 `hm/bw/off` 输出契约。
2. 为本仓库明确安装或暂存完整 TensorRT SDK，包括 ONNX parser 和构建工具；在目标环境构建 epoch-1200 FP32 engine，完成张量、候选和边界对照后再确定发布资产。
3. 开发验证显式指定 Python 路径和版本。需要 ONNX Runtime 做数值对照时，在所选隔离环境补齐该依赖；采用需要 `onnxscript` 的 exporter 时先验证其可用性。
4. 采用 `gui-local10-m0-20261013` 及当前评价器建立新基线；若恢复历史预测与输入哈希，再说明对应关系。需要 HY 精确历史回放时，按 1000 项 manifest 挂载或恢复数据并逐对重哈希。

本次完成资产盘点和文档解析检查；模型效果、性能、CMake 构建、CTest、Qt 原生显示、engine 前向和独立部署均未在本节点验证。
