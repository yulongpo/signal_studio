# 交付后的手动验证

本次未启动新的批量G/T推理。R2–R5少量代表样本证明功能、流程、有限输出及坐标映射，不能宣布L3/L4或真实RF精度通过。旧M4失败、超时、中断及效果报告保留；本轮适配身份为2，必须使用新freeze、新结果目录，不能继续写旧执行目录。

## 先做少量交互复核

从`out/vs2026-qt611-release_bin/SignalStudio.exe`启动，新建测试工程，添加一个生成器WAV/TXT或指定TorchSig SigMF配套样本。只核对这两类原始标签，不把其它格式默认为已支持。导入前确认Fs、Fc、有效带宽、物理长度与原始标签坐标；不要在导入和检测结果上重复加中心频率。

| 操作 | 观察要点 |
|---|---|
| 默认检测后改N=8192、T=3、hop=2ms、滑动=2帧 | 提交后实际计算配置改变；显示缩放/抽帧不改已提交任务 |
| N=3001、T=2、hop=3ms，选择SCN平均谱 | 显示实际FFT重算、模型32768网格、线性功率平均及Hz映射，不要求恢复32768/16帧 |
| 两模型分别选择当前/平均/最大/最小谱 | 所选类型写入任务快照；平均谱不被禁用，不能将所有模式效果视为相同 |
| 短文件、短ROI、边缘ROI、尾部及Partial前缀 | 使用真实可得帧与FFT边缘补零；不复制谱帧、不分析未读区域；记录支持量 |
| 完整文件、可见范围、ROI、活动段 | 实际计算上下文与输出范围分开；任务就绪、运行、取消、失败、正常零结果可区分 |
| 算法对象接受→确认同一对象→CPU DDC→保存重开 | 不复制对象，通道来源快照保留；来源修订后Changed，不静默改旧通道 |
| 原始标签/手动/算法图层和筛选 | 只影响可见性；多段、原始/当前修订、原文、复核和重开一致 |

不规则N是分析配置，模型输入由后端匹配为2次幂且至少2048；显式统计谱的模型时间行为一行，观测仍保留真实积累帧数。当前FFSCN部署profile覆盖范围见README；若超出实际包容量，应读取具体profile/资源原因，不把“模型数学能力”和“本包已覆盖形状”混为一谈。

遇到问题保留整个测试工程、所选文件身份、run/chunk记录、错误原文和截图，不反复覆盖同一结果目录。L3/L4需要独立真值和完整评价；GUI中“Completed”或正常零结果都不表示定位效果达标。

## 创建当前执行身份

以下是本机已有冻结G10/T96数据和epoch1200权重的入口。模板为当前分支M4代表性冻结文件；模板不是本次适配的效果结果。工具先核对旧数据/参考资产不变，包括模板实际`dataAssetsVerifiedBeforeInference`的308项数据与声明数量（兼容旧`datasetFiles`），再更新当前构建输入。`--include-current-inputs`额外纳入所有Git跟踪的源码/脚本/CMake/编译资源以及Debug/Release整个部署目录，包含旧模板中没有的新动态engine和manifest。记录当前HEAD、工作树状态、模板hash和适配版本；不改旧freeze，不改数据hash来迁就变更。

应在最终提交、部署就绪后执行。下列`manual-v1`必须尚不存在；再次开始独立验证时改用`manual-v2`等新路径。此阶段只有文件身份校验，没有模型推理；完整冻结数据仍须可读，即使稍后只选一个诊断文件。

```powershell
$validationPython = 'D:/profiles/anaconda/python.exe'
& $validationPython -B tools/detection/freeze_validation.py `
  --template docs/implementation/wideband-detection/M4/execution-freeze-representative-r4-release.json `
  --output out/tasks/wideband-detection/R6/diagnostics/manual-v1/execution-freeze.json `
  --include-current-inputs
if ($LASTEXITCODE -ne 0) { throw 'Execution identity creation failed' }
```

若旧模板中的文件缺失或不可变资产变化，工具会停止并报告路径；先恢复相同资产或另行建立经审查的数据版本，不能绕过校验。其它机器的绝对路径需要独立迁移清单，本入口不把本机路径当跨机器通用数据包。

## 单文件诊断与完整矩阵

先选一个scope和一个文件确认工具链。`--max-files 1`始终是诊断，不能算完整验收；此处不进行频繁GPU/RSS采样。

```powershell
& $validationPython -B tools/detection/time_suite.py `
  --freeze out/tasks/wideband-detection/R6/diagnostics/manual-v1/execution-freeze.json `
  --root out/tasks/wideband-detection/R6/diagnostics/manual-v1/one-file `
  --configuration Release --scope G-sample --max-files 1 `
  --no-resource-sampling
```

由用户决定何时运行完整矩阵，以下命令不含`--max-files`，使用另一个结果根目录。一次一个native/reference阶段，保持全部样本和失败计账；默认每阶段1500秒保护上限，不是预估耗时或实时性承诺。

```powershell
& $validationPython -B tools/detection/time_suite.py `
  --freeze out/tasks/wideband-detection/R6/diagnostics/manual-v1/execution-freeze.json `
  --root out/tasks/wideband-detection/R6/diagnostics/manual-v1/full-matrix `
  --configuration Release --scope all --no-resource-sampling
```

矩阵仍是6种G配置×10文件、T默认96、T短窗两组各32，共9 scopes/220对。G为生成器文件，T为合成保留集，均不是RF实采。可用`--scope G-roi`等单独执行；完整列表、预算和历史恢复规则见[M4套件](../M4/SUITE.md)。该套件使用既有FFSCN参考配置和product-v2语义，不覆盖SCN及所有新增统计谱/动态形状的精度；后者按上方交互矩阵另行评价，不能套用旧默认数值结论。

同一身份、配置和参数可恢复同一目录，已经完成的失败不会自动重试或被后来成功覆盖。不同代码、模型、数据、范围或freeze必须用新目录。参考或native失败保留全部真值按失败计账；先检查每阶段退出码与原因，再看`evaluation.json`、`scope-summary.json`和`suite-summary.json`。局部诊断、未完成scope或参考失败不建立完整parity。不能从已见结果重新选阈值再宣称独立验证。

## CPU自检及待验证边界

```powershell
& $validationPython -B tools/detection/freeze_validation.py --self-check
& $validationPython -B tools/detection/time_suite.py --self-check
```

以上仅检查身份/历史保护和CPU进程超时处理，不启动Qt、GPU或正式数据推理。freeze自检产物位于R6任务目录；time-suite既有CPU自检沿用M4任务目录。工具不自动构建，不替用户执行大量数据验证。

仍需用户评价：L3/L4完整效果、SCN及各统计谱效果、真实RF真值集、跨设备与长文件吞吐、受控冷/热启动。R5实测Release反馈达建议目标，Debug一次启动921.536ms未达；不要把UI功能通过转述为全部配置性能通过。
