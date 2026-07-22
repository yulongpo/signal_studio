# C++ 接口说明

| 元数据项 | 内容 |
|---|---|
| 文档编号 | SS-API-CPP-001 |
| 文档名称 | C++ 接口说明 |
| 项目名称 | Signal Studio / Signal Platform |
| 文档版本 | V1.0.0 |
| 基线版本 | BL1.0 |
| 状态 | 已批准 |
| 内容类型（meta.contentType） | Reference |
| 编制日期 | 2026-07-22 |
| 适用阶段 | SDK 实现 |
| 输入来源 | 总体架构、模块设计 |
| 本版变更 | 定义公共 API/SDK/ABI 与第三方隔离 |

## 1. 命名空间与目标

`signal::core/data/dsp/compute/task/visualization/workbench/plugin/model/dataset` 分别由同名 CMake target 导出。公共 API 用平台值类型、PIMPL/抽象接口和 `Result<T>`；禁止 Qt、Eigen、oneMKL、FFTW、cuFFT、TBB、ONNX Runtime 类型进入签名。

## 2. API 目录

| API ID | 模块 | 命名空间 | 签名/类型 | 契约 | 稳定性 |
|---|---|---|---|---|---|
| API-CORE-001 | SignalCore | `signal::core` | `VersionInfo version()` | 返回平台、API、ABI 与构建版本 | Stable-1.0 |
| API-CORE-002 | SignalCore | `signal::core` | `Result<T> / Error` | 统一成功/失败模型，错误不可丢失 | Stable-1.0 |
| API-CORE-003 | SignalCore | `signal::core` | `Quantity<Unit, Rep>` | 时间、频率、采样率的强类型量 | Stable-1.0 |
| API-CORE-004 | SignalCore | `signal::core` | `Hash256 hash_file(PathView)` | 文件完整性与缓存键 | Stable-1.0 |
| API-CORE-005 | SignalCore | `signal::core` | `ILogger::write(LogEvent)` | 结构化日志，不暴露 spdlog 类型 | Stable-1.0 |
| API-CORE-006 | SignalCore | `signal::core` | `IWorkspaceStore::load/save` | 公共 Workspace 原子持久化 | Stable-1.0 |
| API-CORE-007 | SignalCore | `signal::core` | `IArtifactStore::commit` | 不可变结果与导出制品提交 | Stable-1.0 |
| API-DATA-001 | SignalData | `signal::data` | `SignalDescriptor::validate()` | 验证实/复、dtype、字节序、单位与范围 | Stable-1.0 |
| API-DATA-002 | SignalData | `signal::data` | `SampleBlockView<T>` | 只读零拷贝分块视图 | Stable-1.0 |
| API-DATA-003 | SignalData | `signal::data` | `IDataSource::read(ReadRequest)` | 异步、可取消、帧对齐读取 | Stable-1.0 |
| API-DATA-004 | SignalData | `signal::data` | `IFormatAdapter::probe/open` | RAW/WAV/HDF5/插件格式适配 | Stable-1.0 |
| API-DATA-005 | SignalData | `signal::data` | `IMultiResolutionStore::get/put` | 概览与瓦片缓存 | Stable-1.0 |
| API-COMPUTE-001 | SignalCompute | `signal::compute` | `IComputeBackend::capabilities()` | CPU/SIMD/CUDA 能力探测 | Stable-1.0 |
| API-COMPUTE-002 | SignalCompute | `signal::compute` | `IBufferPool::acquire(BufferSpec)` | 受预算约束的主机/设备内存 | Stable-1.0 |
| API-COMPUTE-003 | SignalCompute | `signal::compute` | `IBackendSelector::select(Workload)` | Auto 选择和可审计降级 | Stable-1.0 |
| API-DSP-001 | SignalDSP | `signal::dsp` | `IFftBackend::create_plan(FftSpec)` | oneMKL/cuFFT/许可适配器计划 | Stable-1.0 |
| API-DSP-002 | SignalDSP | `signal::dsp` | `IPsdEstimator::process(SampleBlock)` | 窗、ENBW、单位和平均策略明确 | Stable-1.0 |
| API-DSP-003 | SignalDSP | `signal::dsp` | `IStftProcessor::process(StftRequest)` | 分块 STFT 与瓦片输出 | Stable-1.0 |
| API-DSP-004 | SignalDSP | `signal::dsp` | `IResampler::process(Ratio, SampleBlock)` | 抗混叠的成熟库适配 | Stable-1.0 |
| API-DSP-005 | SignalDSP | `signal::dsp` | `IFilter::process(SampleBlock, State)` | 保持跨块状态 | Stable-1.0 |
| API-TASK-001 | SignalTaskRuntime | `signal::task` | `TaskSpec / ResourceProfile` | 冻结的任务、资源和依赖描述 | Stable-1.0 |
| API-TASK-002 | SignalTaskRuntime | `signal::task` | `ITaskService::submit(TaskSpec)` | 提交并返回 TaskHandle | Stable-1.0 |
| API-TASK-003 | SignalTaskRuntime | `signal::task` | `TaskHandle::pause/resume/cancel` | 幂等控制 | Stable-1.0 |
| API-TASK-004 | SignalTaskRuntime | `signal::task` | `ITaskObserver::on_event(TaskEvent)` | 进度、日志、指标的单一状态源 | Stable-1.0 |
| API-VIS-001 | SignalVisualization | `signal::visualization` | `IChartView::bind(IDataSeries)` | 图表与数据提供者解耦 | Stable-1.0 |
| API-VIS-002 | SignalVisualization | `signal::visualization` | `ViewportController::set_time/frequency` | 多图共享视口 | Stable-1.0 |
| API-VIS-003 | SignalVisualization | `signal::visualization` | `SpectrumView / SpectrogramView` | 统一频率轴与原子提交 | Stable-1.0 |
| API-VIS-004 | SignalVisualization | `signal::visualization` | `TimeWaveformView / TimeNavigator` | 当前视窗与实际读入范围 | Stable-1.0 |
| API-VIS-005 | SignalVisualization | `signal::visualization` | `ConstellationView / EyeDiagramView` | 可复用分析视图 | Stable-1.0 |
| API-VIS-006 | SignalVisualization | `signal::visualization` | `OverlayModel::selection/measurement` | Selection 与测量覆盖层 | Stable-1.0 |
| API-WB-001 | SignalWorkbench | `signal::workbench` | `IServiceRegistry::resolve(ServiceId)` | 宿主服务发现 | Stable-1.0 |
| API-WB-002 | SignalWorkbench | `signal::workbench` | `IPanelFactory::create(PanelContext)` | Dock/Inspector/Center 面板 | Stable-1.0 |
| API-WB-003 | SignalWorkbench | `signal::workbench` | `ICommandRegistry::register_command` | 菜单、快捷键与自动化命令 | Stable-1.0 |
| API-WB-004 | SignalWorkbench | `signal::workbench` | `IDiagnosticsProvider::snapshot` | 真实环境和后端诊断 | Stable-1.0 |
| API-PLG-001 | SignalPluginSDK | `signal::plugin` | `signal_plugin_query_v1(host, out)` | 版本化 C ABI 唯一入口 | Stable-1.0 |
| API-PLG-002 | SignalPluginSDK | `signal::plugin` | `IAlgorithmPlugin::describe/run` | 无 UI 依赖的算法插件 | Stable-1.0 |
| API-PLG-003 | SignalPluginSDK | `signal::plugin` | `IFormatPlugin / IExportPlugin` | 格式和导出扩展 | Stable-1.0 |
| API-MODEL-001 | SignalModelRuntime | `signal::model` | `IModelRegistry::install/resolve` | 模型包、版本、哈希和兼容 | Stable-1.0 |
| API-MODEL-002 | SignalModelRuntime | `signal::model` | `IInferenceSession::run(InferenceRequest)` | ONNX 默认后端、设备可选 | Stable-1.0 |
| API-DSET-001 | SignalDataset | `signal::dataset` | `IDataset::query(SampleQuery)` | 版本化样本索引 | Stable-1.0 |
| API-DSET-002 | SignalDataset | `signal::dataset` | `IDatasetWriter::append/commit` | 分片原子提交 | Stable-1.0 |

## 3. 示例

```cpp
namespace signal::task {
struct TaskSpec final {
  core::Uuid task_id;
  core::String task_type;
  ResourceProfile resources;
  core::Vector<core::Uuid> dependencies;
  core::Hash256 idempotency_key;
};

class SIGNAL_TASK_API ITaskService {
public:
  virtual ~ITaskService() noexcept = default;
  virtual core::Result<TaskHandle> submit(const TaskSpec&) noexcept = 0;
};
}

extern "C" SIGNAL_PLUGIN_EXPORT int
signal_plugin_query_v1(const SignalHostApiV1*, SignalPluginApiV1*) noexcept;
```

## 4. 兼容和发布

SDK `MAJOR.MINOR.PATCH`；Plugin ABI 单独整数主版本。Windows ABI 矩阵锁 MSVC toolset/x64/runtime；异常、STL 容器所有权和分配器不得跨插件 ABI。弃用至少保留一个次版本，提供替代 API 和编译警告。包导出 `SignalPlatformConfig.cmake`、targets、headers、symbols、licenses、SBOM 和 examples。

## 5. 线程与生命周期

接口标注 ThreadSafe/UIThread/TaskThread；所有 handle 有明确所有权，回调允许注销并防止退出后调用。耗时调用返回 TaskHandle，不在 UI 线程同步执行。

## 参考资料

- 原始材料：`../references/`（交付目录之外，只读输入）
- 平台任务提示词（平台化架构版）

## 未决事项

- 无阻断性未决事项；正文中的建议值和待确认项继续按其原状态追踪，不因文档获批而视为已实施。

## 变更记录

| 版本 | 日期 | 变更 |
|---|---|---|
| V1.0.0 | 2026-07-22 | 建立并自动审核通过平台化开发基线，纳入需求、接口、测试和复用边界。 |
