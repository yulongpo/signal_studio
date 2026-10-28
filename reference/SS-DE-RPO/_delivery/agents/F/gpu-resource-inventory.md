# GPU resource inventory

设备存在。本文件不把推理或 benchmark 标成通过。

## nvidia-smi

命令：

```text
nvidia-smi --query-gpu=name,driver_version,memory.total,memory.free,compute_cap,pci.bus_id --format=csv
```

本次会话较后的一次输出：

```text
name, driver_version, memory.total [MiB], memory.free [MiB], compute_cap, pci.bus_id
NVIDIA GeForce RTX 5060 Laptop GPU, 591.84, 8151 MiB, 6063 MiB, 12.0, 00000000:64:00.0
```

同一次会话较早的完整 `nvidia-smi`（2026-10-28 19:24:35，本机时间）打印：

- NVIDIA-SMI 591.84，Driver Version 591.84，CUDA Version 13.1
- GPU 0，名称 NVIDIA GeForce RTX 5060 Laptop GPU，WDDM，Bus-Id `00000000:64:00.0`，Disp.A On
- 当时 CSV 的 memory.free 是 6484 MiB；表格 Memory-Usage 是 1417 MiB / 8151 MiB

两次 free 不同，因为桌面和其他进程占用在变。这是采样，不是检测预算。CUDA Version 13.1 是该次 nvidia-smi 对驱动能力的打印，不是本进程加载的 CUDA runtime。

## 代码里的三笔账（CONFIRMED_CODE）

| 字段 | 默认 | 含义 |
|---|---|---|
| `maximumExecutionBatch` | 2 | 合法档 1/2/4/8/16 的 worker 上限。本提交没有改成 16 |
| `maximumEngineBlobBytes` | 128 MiB | 主机上的 engine 文件上限，不是 device 显存 |
| `maximumDeviceBytes` | 4 GiB | runtime 已占用加本次 device 分配的独立上限 |

Device 分配是 input、三个 head 和 workspace 的 `cudaMalloc`。Engine blob 先在主机上读入并做 SHA-256，再 `deserializeCudaEngine`。源码仍要求打包的 CUDA runtime 12.9 与 TensorRT 10.11.0.33，并按 manifest 里的 GPU 名匹配设备。本会话没有加载这些 DLL，也没有核对它们是否仍在磁盘上。

装载路径在 optimization profile 上限已知、并且 `cudaMemGetInfo` 已经成功之后调用选择函数。`freeDeviceBytes == 0` 在函数里表示未知；若 CUDA 真的报出 0 空闲字节，选择函数不按显存缩小，随后的 `validateDetectionModelMemory` 仍会因为分配大于空闲显存而失败，并保留原有 `retryableResourceFailure` 降档。
