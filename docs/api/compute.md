# SignalCompute C++ API

## 公共边界

头文件：`signal_studio/compute/compute.hpp`。公共接口不包含 CUDA、oneMKL、TBB 或 Qt 类型。

## 后端

- `IComputeBackend::capabilities()`：返回后端类型、标识、运行时版本、设备、可用性、操作能力、线程和内存。
- `discover_compute_backends()`：生产探测 CPU 标量/SIMD/多线程及 CUDA。
- `ComputeRuntime::select()`：按交互性、操作量、工作集和设备内存选择后端。
- `ComputeRuntime::fallback()`：记录失败后端、原因、实际后端和 degraded 状态。
- `measure_consistency()`：从独立参考结果与实际结果计算最大绝对误差和均方根误差。
- `verify_consistency()`：仅接受已由独立参考结果计算的指标，并按最大绝对误差/均方根误差阈值写入验证结果；没有独立参考时不得声明一致性已验证。

强制 CUDA 请求在不可用时返回失败，不执行静默降级。自动请求可降级，但必须保留 `requested=cuda` 与失败原因。

## 内存

`BudgetedBufferPool` 统一管理 host、pinned host 和 device 分配器。所有租约计入同一预算；申请校验非零大小、二次幂对齐、溢出和剩余预算。租约元数据构造失败会释放底层分配并回滚预算。设备内存不暴露主机 `span`。

`make_cuda_memory_allocators()` 只在已编译且实机可运行时返回绑定当前实际设备的页锁定/设备分配器，并校验实际地址满足请求对齐。分配器在申请和释放前进入绑定的设备上下文，结束后恢复调用线程原设备。

## 性能辅助契约

`summarize_performance()` 至少接收 30 个正延迟样本，返回 P50、P95、最大值和吞吐。`make_large_file_read_plan()` 根据文件字节数、帧字节数和工作集预算返回有界初始窗、导航就绪状态及估计来源。`ViewActivityGate` 将视图专属活动与共享后台活动分离。
