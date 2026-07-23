# Signal Studio 1.0.0 发布说明

发布日期：2026-07-23

## 概述

Signal Studio 1.0.0 是一个 Windows 离线 IQ 信号分析桌面应用与可复用 C++20 平台。十模块公共平台（Core/Data/DSP/Compute/TaskRuntime/Visualization/Workbench/PluginSDK/ModelRuntime/Dataset）支撑 Signal Studio 应用，复用于信号仿真/批量样本/训练/评估/推理等工具。

## 主要功能

- **数据导入**：WAV 自动解析、SC16 RAW IQ 文件名提示导入、实/复、IQ 交错、int16/位宽/字节序、有界窗口读取、选区导出。
- **大文件与缓存**：分块有界读取、多分辨率索引缓存、不强制全量读入内存。
- **信号分析**：FFT/IFFT、PSD（dB/Hz）、STFT、时域统计、IQ 度量、FIR 滤波、多相重采样。
- **宽窄带联动**：宽带选区->窄带信道提取（数字下变频）、多图共享视口。
- **可视化**：时域波形/功率谱/瀑布图/星座图/眼图、游标/选区/测量、隐藏停止计算、多色阶。
- **任务运行时**：有界资源池、优先级、DAG、暂停/恢复/取消、进度、重试、幂等、制品恢复。
- **插件/模型/数据集**：ABI-v1 插件加载隔离、算法插件、模型注册、JSON 数据集。
- **计算后端**：CPU 确定性 + cuFFT GPU（CUDA 12.4），自动选择与显式降级。

## 计算后端

- GPU：cuFFT Z2Z（CUDA 12.4）经 `IFftBackend` 适配器，解析信号验证（单音 bin/Parseval/IFFT 往返/PSD/STFT）。
- CPU：oneMKL 为 BL1.0 默认 CPU FFT 后端，本机未安装为环境偏差；CPU FFT 接口返回 unavailable，不伪造。

## 系统要求

- Windows 11 x64
- CUDA 12.4+（可选，GPU 加速；缺失时 CPU 回退）

## 获取

- 便携包：`SignalStudio-1.0.0-win64.zip`（25 MB，SHA256 `bcc7f1ba4d92b580d3bd7f34afd55f812965d1aa44e7796ba54087a454960b9a`），解压即用。
- 源码：`https://github.com/yulongpo/signal_studio.git`

## 验证

- 四配置全量 CTest：无界面 126/126、UI(CUDA) 160/160，合计 572/572。
- 需求追踪矩阵覆盖 198 项（160 FR + 38 NFR）。
- 便携包 self-test 退出 0。

## 已知限制

- oneMKL CPU FFT / ONNX Runtime / HDF5 未装：接口就绪，返回 unavailable，不伪造。
- NSIS 安装器未生成（makensis 未装），仅便携包。
- vcpkg manifest 未启用（与系统 Qt 冲突）。

## 许可证

MIT（源码）；Qt LGPLv3、MSVC Runtime Microsoft 许可（运行库随便携包分发）。
