# R2 自检与复审

日期：2026-10-15。分支 `rebuild/a143-native`，前置 R1 提交 `5df99ba`。
本节点模型输入适配、worker解码、物理映射和保存读取已完成自检与代码复审；
本地提交后才开始 R3 数据源及界面接入。没有推送远端。

## 已实现

- 用户确认 FFSCN/SCN 的 `down_nums=11`，模型输入长度须为2的幂且至少2048。
  FFSCN使用不小于分析长度的最小合法网格，SCN适配到现有32768点部署包；
  不规则频率网格内部插值，结果还原原Hz，不增加真实分辨率。
- 保留FFSCN默认真实逐帧输入；平均、最大、最小、瞬时谱均能进入两类模型。
  瞬时谱指检测窗末个真实帧；平均谱使用线性功率平均。所选模式独立保存。
  网络输入与显式模式的SCN筛选、收边共用谱选择函数。
- 后端接收实际时间行，动态设置FFSCN形状，按实际工作集分配显存并选择profile。
  SCN不再要求16个真实谱帧。输入有限值、物理坐标、字节和内存边界继续校验。
- 分离模型head峰ID和分析频率坐标；分析点数无需4整除。原始head按模型宽度
  写盘和重开校验。旧schemaVersion=1仍冻结原网格，不重算历史结果。

`maximumPsdOnly` 旧profile字段为兼容历史身份继续编码；它描述旧默认预处理，
不能覆盖新请求的 `spectrumMode` 或成为禁用其它谱的能力判断。

## 实际运行

| 检查 | 结果 | 证据 |
|---|---|---|
| 自身脚本Release构建 | 通过 | `out/tasks/wideband-detection/R2/logs/release-build-r8.txt` |
| 自身脚本完整CTest | 23/23，90.69秒 | `release-all-tests.txt` |
| Qt UI/offscreen | 94/94、0跳过，QtTest66.604秒 | 同次CTest原始UI输出 |
| Python工具语法与Ruff | 通过 | 本机指定Anaconda解释器 |
| 动态TensorRT/PyTorch原始头对照 | 5/5 | `dynamic-model-probe.json` |
| C++动态后端相同golden对照 | 5/5 | `native-adapted-forward.txt` |
| C++ SCN功率重网格等价控制 | 1/1 | 同上 |
| 两模型各四种谱，N=3001、3真实帧 | 8/8真实前向有限 | 同上 |
| 旧默认包数值、常量、故障清理及SCN B1/B2 | 通过 | `native-legacy-forward.txt` |

原始头对照采用逐元素 `abs(delta) <= 1e-4 + 1e-3*abs(reference)`，
不能只依据最大绝对误差判定。动态原生14项见 `native-adaptation-summary.json`。
测试还覆盖奇数3001点投影保持Hz、当前谱不被旧邻带瞬态替换、四模式SCN CNR，
以及两模型适配head的保存、重开和字节篡改拒绝。

最终FFSCN engine SHA-256：
`4e14e422e7966eb6f554e322c2d390534650e1b8dd0cee6e04f2ed4305cefc03`。
最终manifest SHA-256：
`776ebd3261c063567e443f5b96d5c93a8e50449d9d491bb1242f8c7bf2575786`。
源码字节身份见 `input-snapshot.json`；SCN资产与确认来源见 `SCN_CAPABILITY.md`。

## 复审及边界

只读复审找到并关闭：缩放峰ID冲突、填充区抑制真实峰、临时数组预算遗漏、
旧v1网格解冻、分析点数4整除限制、SCN后处理偷换最大谱。最终复审PASS，
未发现新的P1/P2。复审不替代上述实际运行。

失败构建日志保留在R2任务目录：早期QJson整数转换编译失败已修复；最大
`[2,1,64,262144]` engine构建显存不足，最终两个profile的资源范围见
`DYNAMIC_MODEL.md`。它们均不是通过证据。没有改写M3/M4历史资产或效果结果。

本节点没有改动获批原型布局，也不宣称UI固定参数校验、短文件/ROI/尾部及
Partial整条流程已恢复；这些必须在R3继续实现。当前是张量、适配与工程行为验证，
不证明各谱检出精度相同，不宣称L3/L4或真实RF效果通过。大量数据效果留待交付后
由用户手动验证。
