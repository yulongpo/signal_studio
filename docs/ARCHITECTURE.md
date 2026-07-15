# Signal Studio 软件架构设计

## 1. 文档状态

- 状态：持续演进
- 关联需求基线：SS-SRS-BL-1.0
- 当前适用里程碑：M1
- 最近更新时间：2026-07-15

本文件记录当前已知约束、候选方案和实际决策边界。没有验证证据的方案保持“待验证”，不视为最终架构。

## 2. 架构目标

大文件高性能访问；UI 与耗时计算解耦；多分辨率显示；宽窄带联动；可追溯 DSP 处理链；可取消任务；项目可恢复；模型隔离接入。

## 3. 技术栈

需求基线指定或允许的技术栈：C++17 或更高、Qt 6.11、Qt Widgets、MSVC 2022 x64、CMake、Windows 10/11；Python/PyTorch、ONNX Runtime 和 TensorRT 作为算法扩展。

实际核查结果：当前仓库没有 `src/`、`tests/`、`tools/`、`cmake/`、`third_party/`、`.github/`，没有 `CMakeLists.txt` 或 `CMakePresets.json`。因此尚不能确认主程序入口、Qt Application/MainWindow、测试框架、Qt 版本、MSVC 配置、第三方依赖、CI 或源码模块。

## 4. 总体架构

```text
表现层（Qt Widgets / 图谱交互）
        ↓
应用层（工作区、通道、任务编排、项目命令）
        ↓
核心数据与服务层（数据源、坐标、缓存、任务、结果）
        ↓
DSP 层（读取、频谱、STFT、DDC、滤波、抽取、估计）
        ↓
推理层（ONNX Runtime / Python 进程外服务）
        ↓
持久化层（项目、缓存、结果和导出格式）
```

## 5. 源代码模块现状与规划

### 5.1 已存在模块

当前仓库未发现源码模块、主程序入口或测试目录。

### 5.2 M1 计划新增模块

仅规划当前任务需要的最小边界，实际目录将在构建与测试基础确认后创建：

- `src/core/data`：RAW IQ 格式、文件检查和同步读取核心。
- `tests/unit`：读取核心的单元测试。
- `tests/test_data/manifests`：测试数据参数和校验信息。

### 5.3 后续模块

`app`、`visualization`、`core/cache`、`core/task`、`dsp`、`inference`、`formats`、`export` 等只保留为后续规划，不提前创建目录或接口。

## 6. 核心数据模型

- `DataSource`：文件路径、格式、采样率、中心频率、IQ/QI、大小端和指纹。
- `WidebandContext`：原始数据源、时间范围、视图和信号区域集合。
- `SignalRegion`：时间/频率范围、来源、标注和关联通道。
- `NarrowbandChannel`：区域映射、处理链、通道状态和结果引用。
- `ProcessingNode`：处理类型、参数、输入输出和版本。
- `AnalysisResult`：结果、坐标、处理参数、算法/模型/插件版本和证据。

字段、生命周期和线程安全约束待设计。

## 7. RAW IQ 文件访问与 M1 最小设计

### 7.1 支持范围

首批支持复数交织 IQ（`I0,Q0,I1,Q1,...`）和复数交织 QI（`Q0,I0,Q1,I1,...`）。本轮不覆盖多通道非交织复杂格式。

首批数据类型为 Int8、Int16、Float32；每个复样本由两个分量组成。分量字节数分别为 1、2、4，复样本字节数分别为 2、4、8。Int8/Int16 按有符号整数解释，Float32 按 IEEE 754 单精度解释；幅度缩放由格式参数提供，是否默认归一化待验证。内部计算类型优先评估 `std::complex<float>`，尚未确定。

字节序支持 Little Endian 和 Big Endian；Windows/x86 默认 Little Endian，但必须由格式参数显式表达。

### 7.2 文件参数模型

最小 `RawIqFormat` 需要表达：文件路径、数据类型、IQ/QI 顺序、字节序、文件头偏移、采样率、中心频率、起始时间、幅度缩放、是否复数、文件大小和有效样本数。

### 7.3 样本索引与边界

原始复样本索引、字节偏移、起始样本、请求数量和实际返回数量均使用 64 位无符号整数。文件尾不完整样本和越界请求必须显式返回状态，不得静默补零。

$$
O_{\mathrm{byte}} = O_{\mathrm{header}} + n_{\mathrm{start}} B_{\mathrm{sample}}
$$

其中 `O_byte` 是文件读取偏移，`O_header` 是头偏移，`n_start` 是起始复样本索引，`B_sample` 是每个复样本字节数。所有乘法和加法都必须进行 64 位溢出检查。

### 7.4 同步核心接口草案

```cpp
struct RawIqFormat {
    SampleDataType dataType;
    IqOrder iqOrder;
    ByteOrder byteOrder;
    std::uint64_t headerOffsetBytes;
    double sampleRateHz;
    double centerFrequencyHz;
    double amplitudeScale;
};

struct SampleRange {
    std::uint64_t startSample;
    std::uint64_t sampleCount;
};

struct ReadResult {
    std::vector<std::complex<float>> samples;
    std::uint64_t actualStartSample;
    std::uint64_t actualSampleCount;
    bool reachedEndOfFile;
};

class RawIqReader {
public:
    explicit RawIqReader(...);
    FileInfo inspect() const;
    ReadResult read(const SampleRange& range) const;
};
```

这是可测试的同步核心接口草案，不是已实现 API。本轮不设计完整异步读取接口。错误返回形式、`std::expected`/兼容实现、异常或错误码、调用方缓冲区复用和取消支持均待验证。

### 7.5 合法性检查与错误模型

至少检查文件存在、可读、头偏移不超过文件大小、有效数据长度与复样本字节数的关系、文件尾不完整样本、采样率、起始索引、请求范围和整数溢出。错误类别规划为 `FileNotFound`、`PermissionDenied`、`InvalidFormat`、`InvalidHeaderOffset`、`IncompleteSample`、`RangeOutOfBounds`、`ReadFailure` 和 `ArithmeticOverflow`，具体表示方式待验证。

### 7.6 内存与线程约束

不得完整加载大文件。单次读取大小应受上限控制，内存随请求样本数线性增长而不随文件总大小无界增长。M1 首批采用同步核心读取，但大块读取不得在 UI 主线程执行；Reader 不得直接操作 QWidget，并发实例安全边界待验证。

内存映射、`std::ifstream` 分块读取、Qt `QFile` 和混合方案均为候选，尚无性能证据，不得写成最终高性能架构。

## 8. 时间和频率坐标

原始样本索引为坐标基准，时间可按

`t = t_0 + n / F_s`

换算；绝对频率和基带频率需区分，重采样必须保留映射关系，滤波群时延必须在结果和坐标中明确补偿。误差目标以 `SS-ACC-001` 至 `SS-ACC-004` 为准。

## 9. 多分辨率索引和缓存

候选内容包括时域 min/max/RMS、频谱概要和 STFT 瓦片。缓存键至少需要包含数据源指纹、范围、分辨率、算法参数和版本；必须定义失效、容量管理、版本迁移和损坏恢复。方案待验证。

## 10. 图谱渲染

需要支持屏幕像素聚合、数据抽稀、渐进式显示和交互反馈。CPU、GPU、Qt RHI、OpenGL 或自定义渲染均为待验证候选；UI 线程只负责交互和提交，不执行大规模计算。

## 11. 宽带分析

以原始 IQ 或大时间范围为输入，提供时域、频谱、STFT 浏览、测量、游标、选区和信号区域管理。具体视图数据协议待设计。

## 12. 窄带分析

从信号区域创建通道，支持 DDC、滤波、抽取、重采样、精细图谱、参数估计以及后续识别和解调。M2 前不预先固定全部算法实现。

## 13. 宽窄带联动

宽带区域可创建一个或多个窄带通道；时间、中心频率、带宽和结果可双向定位或回写。联动状态模型和冲突处理待验证。

## 14. DSP 处理链

候选节点包括去直流、IQ 校正、DDC、滤波、抽取、重采样、参数估计、同步、识别和解调。每个结果必须记录输入范围、处理参数、算法版本和坐标映射。

## 15. 任务调度和并发

UI 主线程负责状态和交互；耗时读取、计算、缓存和导出在工作线程或独立进程执行。任务应具备优先级、取消、过期结果丢弃和资源预算；模型服务可独立进程运行。

## 16. 项目保存与恢复

项目需保存数据源、格式、区域、通道、处理链、参数、结果引用、布局和版本信息，并支持相对/绝对路径、指纹、迁移、自动保存和缺失依赖降级。格式待设计。

## 17. 模型推理

ONNX Runtime 和 Python/PyTorch 服务作为扩展候选。Python 服务应独立进程，通过 IPC 提供启动、健康检查、超时、重启、日志和 CPU 回退；未完成预研前不固定协议。

## 18. 插件扩展

插件范围可覆盖格式、DSP、估计、识别、解调、协议和导出。公共接口应版本化；不可信或 Python 插件优先进程隔离，ABI 和协议边界待验证。

## 19. 错误处理与诊断

输入非法、依赖缺失、任务失败、缓存损坏、模型/插件崩溃和项目恢复失败必须产生可操作的中文反馈，并保留日志和诊断证据；单个任务失败不得导致主程序退出。

## 20. 性能设计

需验证首帧时间、P95 UI 延迟、FPS、100 GB 文件内存上限、取消响应、缓存命中和稳定性，目标以 BL1.0 的 `SS-NFR-*` 条目为准。

## 21. 核心接口

当前只确定以下接口方向：数据块读取、坐标转换、视图数据请求、任务提交/取消、处理节点执行、结果回写、项目保存/恢复和推理健康检查。详细 API 待实现前设计。

## 22. 待验证技术问题

1. 超大 IQ 文件访问方式及内存预算。
2. 图谱渲染后端和渐进式显示策略。
3. STFT 瓦片缓存格式、压缩和失效规则。
4. 任务取消和过期结果处理。
5. Python IPC 与 CUDA 环境隔离。

## 23. 关联需求

大文件：`SS-BIZ-007`、`SS-NFR-002`、`SS-NFR-006`；交互与渲染：`SS-NFR-001`、`SS-NFR-003`、`SS-NFR-004`；任务：`SS-NFR-005`；项目：`SS-PRJ-001` 至 `SS-PRJ-007`；正确性：`SS-ACC-001` 至 `SS-ACC-007`；模型与插件：`SS-AI-*`、`SS-PLG-*`。
