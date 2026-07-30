# MS-4.5 依赖与边界审计

## 审计结论

现有批准依赖能够完整满足 MS-4.5，未新增第三方库，也未修改 `vcpkg.json`、依赖锁、
许可证清单或运行时闭包。

| 能力 | 复用实现 | MS-4.5 薄层职责 |
|---|---|---|
| CPU FFT | oneMKL DFTI 与既有计划缓存 | 参数校验、帧/补零、归一化和结果编排 |
| CUDA FFT | cuFFT Adapter | 同一公共契约、后端来源和 CPU/CUDA 比较 |
| FIR/卷积 | oneMKL VSL Adapter | 分析前 ProcessingChain 快照 |
| IIR/线性求解 | oneMKL LAPACKE Adapter | 滤波与 Savitzky-Golay 系数编排 |
| 重采样 | 既有 libsamplerate Adapter | 本里程碑不新增重采样业务 |
| 并行/计算选择 | SignalCompute 与 TaskRuntime | 预算、取消、最新结果提交 |
| UI | Qt 6.11 Widgets、Designer、Workbench | 参数编辑和显示映射 |

Eigen 与 TBB 仍由现有平台依赖策略管理，本里程碑没有引入需要其直接参与的新数值核。
公共头未暴露 oneMKL、cuFFT、libsamplerate、Eigen、TBB 或 Qt 类型。

## 禁止重实现审计

- FFT 只调用 DFTI/cuFFT；
- FIR/IIR 和分析预滤波只调用既有 ProcessingChain/Adapter；
- Savitzky-Golay 的线性求解与卷积分别调用 LAPACKE/VSL；
- 未实现新的重采样、通用卷积、通用矩阵求解或并行运行时；
- 窗系数、归一化、平均、最大保持和指数递推属于参数编排薄层，不替代成熟内核。

## 后续里程碑边界

- MS-05：Selection 建通道、DDC、重采样输出、通道继承和宽窄带联动；
- MS-06：PluginSDK、ONNX ModelRuntime、Dataset、识别和解调；
- MS-07：正式打包、SBOM、发布级文档收口；
- MS-08：第二薄壳应用复用证明；
- MS-09：最终验收、标签和 GitHub Release。

本里程碑没有创建上述页面、任务、模型、插件或输出，也没有以预滤波快照提前实现
MS-05 通道处理链。
