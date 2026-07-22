# Python 接口说明

| 元数据项 | 内容 |
|---|---|
| 文档编号 | SS-API-PY-001 |
| 文档名称 | Python 接口说明 |
| 项目名称 | Signal Studio / Signal Platform |
| 文档版本 | V1.0.0 |
| 基线版本 | BL1.0 |
| 状态 | 已批准 |
| 内容类型（meta.contentType） | Reference |
| 编制日期 | 2026-07-22 |
| 适用阶段 | Python SDK |
| 输入来源 | C++ API、平台任务 |
| 本版变更 | 定义 Python/NumPy/PyTorch/任务/模型复用 |

## 1. 包与范围

包名 `signal_platform`，首版提供 data、dsp、task、model、dataset 子模块；训练框架保留在应用层。pybind11 私有封装 C++ SDK，Python 侧不暴露 Qt 对象。

```python
import signal_platform as sp
source = sp.open_signal("capture.sc16", descriptor)
view = source.read(samples=(0, 1_000_000))   # NumPy 只读/可控零拷贝
task = sp.submit_task(sp.PsdRequest(source=view, fft_size=8192))
result = task.result(timeout=30)
dataset = sp.load_dataset("dataset://demo@1.0")
pred = sp.infer("model://classifier@2.1", dataset.batch(32))
```

## 2. 内存与互操作

NumPy 通过 buffer protocol 交换实/复数组，shape/stride/dtype/endianness 明确；生命周期由 owning capsule 保证。PyTorch 通过 DLPack 为 Preview，设备/stream 必须显式。不得把可写 NumPy 视图指向只读源映射。

## 3. 任务与错误

同步方法仅限轻量元数据；IO/DSP/推理返回 awaitable TaskHandle，支持 status/progress/pause/resume/cancel/logs。C++ Error 映射稳定 Python 异常类并保留 code/details/recovery。

## 4. Wheel 与兼容

支持 CPython 3.11–3.13 x64（建议基线），wheel 名含平台 ABI；每个 wheel 在干净环境运行导入、NumPy、任务、数据集和推理 smoke test。Python API 遵循 SemVer，Preview 能力显式标注。

## 参考资料

- 原始材料：`../references/`（交付目录之外，只读输入）
- 平台任务提示词（平台化架构版）

## 未决事项

- CPython 精确支持矩阵需与企业运行环境在 MS-00 确认。

## 变更记录

| 版本 | 日期 | 变更 |
|---|---|---|
| V1.0.0 | 2026-07-22 | 建立并自动审核通过平台化开发基线，纳入需求、接口、测试和复用边界。 |
