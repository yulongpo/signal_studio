# SCN 模型输入能力与资产边界

用户于 **2026-10-15 确认 SCN 与 FFSCN 的 `down_nums=11`**。本应用采用的
模型输入频率长度契约为：长度是 2 的整数次幂，且不小于 `2^11=2048`。
不规则分析长度由内部重算 FFT 或插值适配，不能因不等于旧导出值就拒绝
用户的检测请求。

## 当前可用引擎

本次只读检查当前分支已有的
`out/tasks/wideband-detection/M3/assets/controls/scn_model.engine`，并经许可
核对 `D:/project/SCN_based_PSD_Detection/models/scn_model.engine`。两者
SHA-256 相同：
`ae89d04332758d466846b71afc849c446500a82a8bd74fd8c671927247746627`。

实际 TensorRT I/O 为：

| 用途 | 实际张量名 | float32 形状 |
|---|---|---|
| 输入谱 | `resnet_32_input:0` | `[1,32768,1,1]` |
| 带宽 BW | `Identity:0` | `[1,8192,1,1]` |
| 热图 HM | `Identity_1:0` | `[1,8192,1,1]` |
| 中心偏移 off | `Identity_2:0` | `[1,8192,1,1]` |

输入的频率轴是索引 1，输出的频率轴也为索引 1；外部频率解码步幅为 4。
该引擎只有一个固定 optimization profile，min/opt/max 全部是
`[1,32768,1,1]`。因此 **32768 是当前可用序列化引擎的部署规格，不是
SCN 网络必须固定32768点的结论**。当前后端可把用户选择的谱内部重网格
到32768点，并将检测边界映射回原始物理频率；32768满足上述模型输入契约。

## 结构取证与无法证明的事项

现有引擎有184个优化层；inspector 可见18个 `Resize__` 节点，残余层名含
`conv2_block` 至 `conv11_block`。但其 profiling verbosity 只有
`LAYER_NAMES_ONLY`，没有各层 stride、Resize scales/sizes 或中间形状。
**这些残余名称不足以反推出实际 `down_nums`；本次11层的依据是用户确认。**
不能把输入32768反推成15层，也不能仅凭18个Resize节点认定可直接动态化。

当前分支 M3 assets 和获准参考库 `models` 均没有 SCN 原始 ONNX、训练权重
或可供重新导出的模型图；包含 ignored 文件的搜索只找到 FFSCN-17 ONNX。
仅凭当前固定 TensorRT engine，无法可靠重新导出动态 ONNX，也无法核验
Resize 是否由固定尺寸常量控制。因此，本轮继续使用现有32768后端规格与
内部频率适配；若将来取得原始模型资产，再核对逐级 stride 和 Resize 图后
决定动态导出方案。本次没有重建模型或变更历史资产。

两次只读调用的相关 stdout 转存到：
`out/tasks/wideband-detection/R2/diagnostics/scn-engine-inspector-stdout.txt`。
该文件是已执行命令输出的转存摘录，不是重新运行产生的报告，也不是完整
ONNX 图或包含全部184层参数的 engine inspector 导出。

## 谱类型与验证边界

该引擎输入只有一条 float32 频率谱，没有16帧时间轴，也没有最大、平均、
最小或瞬时谱的类型输入。四类谱可由应用按用户选择构建，进入同一归一化
和频率适配路径；所选类型及实际观测支持应进入任务快照和结果证据。

历史 M3 的“16帧最大谱、平均谱推理调用数为0”仍准确描述当时采用的
应用预处理及验证条件，不能改写成当时已经验证四类谱，也不能提升为网络
只允许最大谱或必须16帧的结构限制。本次资产检查没有运行新的效果验证，
不声明平均、最大、最小、瞬时谱的检出效果均已通过。
