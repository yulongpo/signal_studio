# M4 独立参考、评价器与 holdout 工具

三个工具在本仓库独立编写，按 `VALIDATION_PROTOCOL.md` 执行。只读数学依据为 M0 已冻结的 F06–F10；不导入或执行外部 GUI 业务模块，不读取 C++ 的 raw head 作为完整链 reference。M3 已有本仓库 `model_ffscn.py`、`model_frequency_parity.py` 的独立模型定义、解码、唯一窗口关联与频率数学作为可复用依赖，代码身份随 execution freeze 固定。

## 文件与运行约束

- `tools/detection/time_reference.py`：完整 WAV→实际源网格 STFT→独立 PyTorch epoch-1200→解码/跨窗关联/频率测量→sample/ROI/coarse/None→精修后仅重叠/接触去重。输出每窗 head 与输入 hash、真实支持、时频事件、未知观察与逐 track 精修证据。
- `tools/detection/time_evaluation.py`：严格 keyed TXT 或私有 sample-exact truth，读取 reference 与生产 CLI 结果；每 TF 阈值独立最大匹配数再最大 IoU，失败零预测、未知时间不伪装事件、配对文件 bootstrap、短突发/CNR 分层、边缘 quantile 与纯噪声曝光率。
- `tools/detection/time_holdout.py`：先封存完整 96 项参数网格/代码/PRNG/磁盘预算，再分块生成双通道 PCM16 WAV、生成器格式 TXT 与**仅供评价的私有 sample-exact truth JSON**。该 JSON 不扩大产品标签适配范围。

正式 reference/evaluator 必须先收到主任务封存的 `signalstudio-m4-execution-freeze-v1`，`readyForInference:true`；`files` 是 `[{path: absolute,sha256:...}]` 或绝对路径→SHA map。工具先复核所有列入身份，并要求自身源码也列入，再读取正式结果。主任务需包括三个工具、模型/频率 helper、checkpoint/engine/runtime、C++ CLI/worker、协议、完整 IQ/truth manifest 和运行参数。当前 `tooling-manifest.json` 只是准备完成的源码清单，`readyForInference:false`，不能当作整套 runtime 已冻结。

在收到主任务 Go 之前没有运行正式 G/T 或 GPU。诊断中完成了 CPU 数学、codec 与临时生成器单元检查；它们不属于模型效果。

## 参考链调用

```powershell
& 'D:/profiles/anaconda/python.exe' -B tools/detection/time_reference.py `
  --file '<absolute-WAV>' `
  --output 'out/tasks/wideband-detection/M4/diagnostics/reference/G/<sampleId>/sample' `
  --checkpoint 'D:/project/FFSCN_train2/FFSCN_TEST_GUI/config/epoch_1200.pth' `
  --strategy sample --device cuda:0 `
  --freeze 'docs/implementation/wideband-detection/M4/execution-freeze.json'
```

`--strategy` 支持 `sample|roi|coarse|none|no-split`；`no-split` 是 sample 的禁拆分配置。`--short` 采用单独 `.5 ms/1 ms/.1 ms` 精修参数，默认仍是 `5 ms/10 ms/1 ms`。`--center` 默认2.5 GHz，与生产 CLI 相同。初期只接全有效带宽20 Msps stereo PCM16、至少10个真实时间槽，拒绝其它未验 shape。

默认 `--semantics gui-frozen` 固定原参考数学。用户批准的参数扩展可用 `--semantics product-v2`，另外记录 `signalstudio-refinement-m0p-parameters-v2`：ROI 门限乘 `edgeRatio/.6`、mask 奇数平滑核按批准公式变化；用户 noSplit 时 sample 使用固定 `.5 ms` reliability、ROI使用1 pooled cell，不应用可编辑 minOn/minOff；continuous sample 的 edgeFraction 为 `.5×userEdge/.6`。`--edge-ratio` 和 `--smooth-ms` 可显式指定。默认 `.6/1 ms/sample` 的 G 主比较保持原数学；产品扩展结果不得称为非默认 GUI 数学完全等价。批准记录见 `PARAMETER_DECISION.md`。

STFT 使用32帧一组的真实连续 WAV 读取、centered periodic Hann、CUDA或CPU向量 FFT，保存磁盘 float32 spool，最多只把两组10帧实际上下文输入模型。所有完整帧含源样本/padding支持；尾模型窗末端锚定一次，不重复帧。全局10 percentile 用有界 ordered-float32 radix 选择和 NumPy2.4 float32 lerp；不抽样。spool 默认完成后显式关闭并删除；`--retain-stft` 可保留诊断。长文件 spool 磁盘需求按实际帧数检查。

sample 精修读取整个真实 ±8 ms ROI，做 complex64 局部相位、exact-length raised-cosine FFT 低通、冻结 Torch float32 cumsum。reference 默认单块设备/工作预算2 GiB（`--sample-budget-mib` 1–4096），用64 bytes/source sample 的保守估计拒绝超额 ROI；超额保留 GUI fallback parent，附理由。该离线参考预算与 C++ 的256 MiB working / CLI128 MiB evidence不同，报告保留差异，不能删掉 C++ 资源失败来追求同数值。

每个 sample track 同时保存 frozen cumsum 与 float64 accumulator 模拟的 maxAbs、超冻结容差数、mask差异与整数 runs。模拟比较帮助定位已声明数字版本差异，不能替代实际 native energy parity；保持冻结容差，不根据结果扩大。ROI 在指定 device 上做 float32 bilinear/profile，与原数学的三次减均值后一次 ReLU一致。

`result.json` 为 `signalstudio-m4-independent-time-reference-v1`，包含 `pass/events/observations/refinementStates`、文件/代码/模型身份、sampleRate、center、physicalSamples、strategy/short/semantics、支持与时间。`pass` 只表示完整参考计算成功，不表示精度通过。None 的观察不生成精确时间事件。原参考 fallback parent span 保持明确 fallback；与 native 只发布未知窗口的差异将真实进入评价，不能暗中补成 native 的精确事件。

## 评价调用与索引

```powershell
& 'D:/profiles/anaconda/python.exe' -B tools/detection/time_evaluation.py `
  --index 'out/tasks/wideband-detection/M4/diagnostics/G-sample-index.json' `
  --freeze 'docs/implementation/wideband-detection/M4/execution-freeze.json' `
  --output 'out/tasks/wideband-detection/M4/diagnostics/G-sample-evaluation.json'
```

索引一项一个文件，一次只比较同一个配置；示例结构：

```json
{
  "parameters": {"strategy": "sample", "shortProfile": false},
  "expectedTruthCount": 594,
  "samples": [
    {
      "sampleId": "sample-id",
      "group": "G",
      "sampleRateHz": 20000000,
      "centerFrequencyHz": 2500000000,
      "physicalSamples": "16000000",
      "effectiveBandwidthHz": 20000000,
      "source": {"path": "absolute.wav", "sha256": "frozen-sha"},
      "truth": {"path": "absolute.txt", "sha256": "frozen-sha"},
      "referenceResult": "absolute-reference/result.json",
      "nativeResult": "absolute-native/result.json"
    }
  ]
}
```

source/truth hash 再核对，禁止重复/空索引；G 必须10文件/594事件完整覆盖。native result envelope为 schemaVersion1，run为 `signal-studio-detection-run` version1或2；v2进度字符串与上限验证。durable kinds明确接受 `frequency-annotation-v1`、`refined-annotation-v1`，不静默忽略未知 schema/kind。只接 Algorithm且无人工 revision的原结果，频率坐标明确换到RF；Unknown/WindowObservation不成为预测事件。missing/failed/invalid结果按零预测计入并留理由。

三阈值的 TP/FP/FN、配对结果与所有失败文件保留。总体/分组先累计 counts 再计算 micro P/R/F1；PCG64 seed2026101405进行10,000次 paired-file bootstrap，重算 pooled F1差值，报告95%区间。G主门槛点差≥−.01与CI分别保存，reference失败不能建立迁移 parity。短专集单列1–8 ms与6/20 dB组、matched start/end/combined edge median/P95、漏检与默认配置；sample short和ROI short分别评，不混合作主值。

## holdout 计划、生成与资源

```powershell
& 'D:/profiles/anaconda/python.exe' -B tools/detection/time_holdout.py plan `
  --root 'out/tasks/wideband-detection/M4/assets/holdout' `
  --freeze 'docs/implementation/wideband-detection/M4/generator-freeze.json'

& 'D:/profiles/anaconda/python.exe' -B tools/detection/time_holdout.py generate `
  --freeze 'docs/implementation/wideband-detection/M4/generator-freeze.json' `
  --manifest 'out/tasks/wideband-detection/M4/assets/holdout/manifest.json'
```

grid为协议9组/96文件，seed2026101404，fileSeed=base+1000×groupIndex+fileIndex；所有中心/BW、activity source样本、carrier FFT周期、CNR、噪声类型、Fs/count显式写入 generator freeze，代码hash在生成前固定。生成和plan均预检磁盘；一次256K source samples，最大8 s临时文件用complex64 mmap，两遍写出文件级固定缩放PCM。禁止覆盖既有样本或失败残留；正式96对及私有真值全部成功/hash完后才发布 complete manifest，之后主任务才运行推理。

波形是独立随机QPSK频域系数构成的周期带限 noise-like carrier，再用样本精确矩形包络开关。名义频带是开关前的FFT支持；包络会导致频域泄漏，不能称名义BW为实测occupied BW。CNR在组件活动样本平均功率上标定到**期望白噪声名义带内功率**，排除其它载波；不是实测总干扰CNR。TXT class=`SYNTH_NOISE`准确标明合成类型，不把模型输出说成真实调制识别。每文件缩放保证I/Q分量峰值≤.8满量程，不clip；小数TXT与sample-exact JSON分别留身份。

主任务实际执行生成之前还应复核其 Go 与 execution freeze；本工具交付阶段只运行plan，尚未生成正式96文件。没有真实RF采集子域，不用合成数据填补该能力声明。

## 已完成的检查与边界

三个文件经过 `python -m black --line-length 80` 与 `py_compile`。CPU self-check覆盖 cardinality优先的反例/逐阈值、failed truth保留、配对bootstrap、fine先删短run再合并与等值、不拆分、精确有界分位、局部相位低通、常量ROI。

`out/tasks/wideband-detection/M4/diagnostics/tool-cpu-checks.json` 额外记录：严格读取G标签10文件/594事件；seed1可删除的小型codec样例验证WAV/TXT/私有sample-exact真值、runv2/refined kind、Unknown观察与失败保留真值。一次100万cell CPU算术模拟的float32cumsum与float64 accumulator最大差为 `2.444721758365631e-08`，该case超冻结容差数与mask差异均为0；它不证明实际native/G全链相等。未运行正式G/T、CUDA前向或准确率评估，主任务另行调度。
