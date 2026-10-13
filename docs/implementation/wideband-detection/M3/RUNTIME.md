# M3 原生模型运行时

本节点把真实 TensorRT 推理放在独立的 `SignalStudioDetectionWorker` 进程中。模型前向使用 NVIDIA CUDA；主窗口图谱的 QRhi Direct3D11 渲染仍是另一条运行链路。链路夹具明确保留 `pipeline-fixture-v1`，不能当作模型检测结果。

## 包与预检

`tools/detection/model_package.py` 使用 Python 标准库生成可迁移包，优先由 `D:/profiles/anaconda/python.exe` 执行。两份正式模型位于部署目录的 `detection-models/<profile>/manifest.json`；引擎与该清单在同一目录。冻结的 TensorRT 10.11.0.33 与 CUDA 12.9.79 DLL、许可证分别部署到运行目录及 `detection-models/`。运行不需要 Python、训练工程或 SDK 搜索路径。

包清单使用 `signalstudio-detection-model-package-v1`。预检校验 profile/version、FP32、模型字节数与冻结 SHA-256、输入/输出名称、float32 连续布局、形状、TensorRT/CUDA 版本以及设备要求。请求中的 `modelSha256` 必须非空并与包相等。引擎在读取后重新核对实际字节 SHA，才进入反序列化。CUDA/TensorRT DLL 也按冻结 SHA 检查；从部署目录延迟加载，避免模型 DLL 缺失时阻断显式夹具 worker 启动。

FFSCN 主线为 epoch-1200 FP32，引擎 SHA `d2de250b11d6bc4a3ccec6d95a232b03ba011e085698d525c3454387094bfe81`。模型输入为 `[B,1,10,65536]`，真实 B1/B2，输出语义顺序为 hm/bw/off，各 `[B,1,1,16384]`。

SCN 频率对照引擎 SHA `ae89d04332758d466846b71afc849c446500a82a8bd74fd8c671927247746627`。实际引擎为固定 `[1,32768,1,1]`，BW/HM/off 的真实名称分别是 `Identity:0`、`Identity_1:0`、`Identity_2:0`。应用仍可提交逻辑 B2，后端保持原模型形状并按成员顺序执行两次 B1。每个成员只有十六帧最大谱的 minmax 结果进入模型；均值谱不进入模型。该配置是频率对照，不带准确率承诺。

FFSCN 17 阶配置固定的 131072-bin 数值一致性没有通过；预检明确拒绝。不改成较小输入，不自动选择 FP16 或 ONNX CPU 路径。

## 运行边界与证据

引擎是指定 NVIDIA GeForce RTX 5060 Laptop GPU、SM 12.0 与对应 TensorRT 版本生成的设备包，不声称硬件或版本通用。运行时检查实际 GPU、CUDA 运行时/驱动，输入和三头的 float32、设备位置、连续 strides 及 finite 值。

资源检查遵循规划 `03_ARCHITECTURE_AND_CONTRACTS.md` 第 77 行的边界：任务 `workingSetBudgetBytes` 初始 256 MiB 只约束 CPU 中间工作集，模型和设备另外预检。CPU 预留覆盖源输入、转换输入及输出、打包头、binary、wire 四份完整头副本，记入 `hostWorkingSetBytes`；暂时驻留的序列化引擎另受 128 MiB 资产缓冲上限约束，记录 `engineBlobBytes`。不将 CUDA 字节与 CPU 预算相加。

本节点选用独立 4 GiB worker 设备预算作为初始部署上限，后续 M5 依据实际 RSS/VRAM 和长运行结果评估。该上限与任务的 256 MiB CPU 阈值分别保存；显式 CUDA 输入、三输出和 workspace 分配仍必须小于当前实际可用 VRAM。实际 TensorRT 初始化前后可用显存差记入 `runtimeDeviceBytes`，显式 CUDA 分配总量记入 `boundedDeviceBytes`，两者合计不得超过设备上限。输入/输出最低分配已经超过设备上限时，在加载引擎前拒绝；设备不足或 CPU 超预算均明确失败。

证据另列 `deviceInputBytes`、`deviceHeadBytes`、`deviceWorkspaceBytes`、设备可用/总显存和三个独立预算，live 测试直接打印两种模型的 JSON 资源证据。CPU 预留量、引擎 blob 大小和 CUDA 分配记录不能替代后续 M5 的进程 RSS/VRAM 长运行实测；TensorRT 内部 host heap 不被虚构为已精确测量。

每次前向的 stream drain 作用域在所有本机输入/输出 vector 创建后建立，保证成功、异步拷贝失败、enqueue 失败及输出检查失败都先完成已入队的 CUDA 工作，再释放缓冲区。原始错误不被清理操作覆盖。live 测试在三个输出分别完成入队后注入失败，检查调用方输出未变、drain 已执行，以及同一后端后续前向可用。

TensorRT `ILogger` 可以由内部工作线程调用。后端日志写入与错误读取共用 mutex；读取在锁内复制 QString，禁止直接访问可变日志字段。单条消息最多读取 4096 UTF-8 字节再转换，避免先转换无限长消息后截断。

worker begin 返回实际 backend、设备、运行时、精度、模型/清单 SHA 与批量限制。每个 result 保留兼容的 `windowId/candidates` 结构、解码统计和模型证据，附带 `[B,3,1,W]` float32 原始头（hm/bw/off 顺序）。生产结果明确使用 `candidateEncoding=raw-heads-v1`，JSON 中的 candidates 是空数组；主进程以验证后的原始头和同一请求参数重建完整候选并写持久化分片。这样 B2/TopK512 不会重复成超限 JSON，也不会降低参数或丢弃框。JSON 控制块继续遵守 M2 的 64 KiB，binary 不超过 32 MiB；超额结果整体失败并返回明确错误。

## 自检入口

- `signalstudio_configure_detection_model_backend(target)` 接入可选、精确 SDK；没有 SDK 的构建保留夹具，真实模型明确不可用。
- `tests/detection_model_backend_tests.cpp` 覆盖最大谱/均值区别、常量归一化、批量隔离、输入形状、finite、预算、manifest 改写/路径、模型 SHA、未就绪前向及实验配置拒绝。
- 同一测试的 `--live <model-root> <golden-root>` 读取三个独立探针的 B1/B2 PyTorch 参考头，并检验真实 SCN 固定 B1/逻辑 B2 的一致性。
- `model-deployment-debug.json` 记录本机 Python 实际复制和哈希复核结果；这是包验证，不是前向或应用准确率证明。主 Agent 后续通过项目 `scripts` 入口统一构建、自检与截图，并归档真实结果后再完成 M3 本地提交。

本文件是实现契约和入口记录。完整 M3 成败以主 Agent 的节点报告、测试日志及实际截图为准；此处不将尚未执行的 C++ 构建/前向写成已通过。
