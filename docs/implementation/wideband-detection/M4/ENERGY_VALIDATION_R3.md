# M4 真实文件局部能量对照 R3

三个配置在读取正式结果之前固定，见 `out/tasks/wideband-detection/M4/diagnostics/energy-configs/index.json`。R3 冻结了配置、文件、C++ energy CLI、domain、参考源码和运行环境；使用用户指定 `D:/profiles/anaconda/python.exe`，独立 Torch CUDA 参考与实际 C++ 结果顺序运行。它们是生成 IQ 文件，不能称实采 RF。

| 固定范围 | 实际 FFT 长度 | power/smooth 超冻结容差数 | mask 差异 | 最终整数边缘 | strict parity |
|---|---:|---:|---:|---|---|
| G 第一文件、预选载波频带 | 1000000 | 0 / 0 | 0 | 完全相同 | PASS |
| G 第二文件、任意长度局部范围 | 499999 | 0 / 0 | 0 | 完全相同 | PASS |
| T08-00、预选短突发频带 | 800000 | 0 / 0 | 25 | 8 段数量相同；最后一段 end 相差 1 source sample（50 ns） | FAIL |

power、平滑包络和门限的容差仍为 `atol=1e-7, rtol=1e-3`。T 的 25 个差异均位于预先定义的门限临界区域，非临界差异为 0；该分类只用于解释，不取消 strict mask/整数端点失败。不能用 50 ns 小于 L4 的 1 ms 目标抵消离散 parity 失败。

归档的三个 `energy-parity-r3/*.json` 包含数组、门限、状态/原因、源整数边缘、代码/配置/输入 SHA 与差异索引 SHA。完整 f32 数组和临界样本索引留在对应 `out/tasks/wideband-detection/M4/diagnostics/energy-release-r3/`，位置与身份由报告指向。数组结果说明这些固定范围的 exact-length CPU 带限功率可与独立参考比较，不能代表全部 G/T、ROI 池化或模型效果。

早期 9999 点合成 CPU 诊断中的 f64 rolling 与冻结 Torch f32 cumsum 超差仍保留为 FAIL。该结果没有因本次 G 局部范围通过而被替换；当前产品独立数字版本与参考差异继续公开报告，不调整冻结容差。

完整模型链的 L3/L4 按 R3 的 220 对矩阵另行评价；本报告不是效果验收结论。
