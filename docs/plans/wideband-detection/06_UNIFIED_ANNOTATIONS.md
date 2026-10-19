# 统一标记：记录、导入、交互与展示

状态：待审核。本期核心工作，与宽带检测一起交付。已按用户最新要求将原始标签适配限定为 **broadband_signal_generation 输出**和 **TorchSig 指定数据集的 SigMF**。参考仓库为 [TorchDSP/torchsig](https://github.com/TorchDSP/torchsig)；不以 torchsig-models 作为来源。本次只读样本，不开发解析器。

## 1. 一个对象、三种来源

现有 Mark 只有 id/name/range，无法区分原始标签、人工输入和算法输出，也不能保存多突发、修订和模型证据。拟以 Annotation 为统一实体，兼容现有 Mark 操作入口；不维护一份算法框和一份接受后 Mark 的重复真值。

| 结构 | 必要内容 | 不变量 |
|---|---|---|
| AnnotationSourceSet | id、fileId、样本通道、来源类型、格式/profile 版本、原始文件/运行引用、摘要、导入诊断 | 一个文件可挂多个集合；同名不同内容不自动覆盖 |
| Annotation | id、sourceSetId、originRecordId、名称、类别、活动段、当前 revisionId、复核状态 | 原始标签/算法标记被人工编辑后 sourceKind 仍保持来源 |
| Segment | segmentId、样本半开区间、频率上下界、时间/频率语义、边界不确定度 | 多突发保留多段，不用跨空隙的大矩形冒充连续发射 |
| OriginEvidence | 原始记录、原始几何、观察窗口、来源字段、坐标转换、模型分数及证据引用 | 导入或推理完成后不可变；恢复原始值不依赖外部文件仍在原路径 |
| AnnotationRevision | revisionId、parentRevisionId、动作、前后值/差异、操作者、时间、说明 | 每次提交是一条修订；撤销/重做通过命令维护有效版本 |
| Review | Pending / Accepted / Rejected、决定时间、针对的 revisionId | 复核状态不改变来源；修订边界后原接受状态需重新确认 |
| ChannelSource | annotationId、segmentId、revisionId、创建时范围快照 | 标记变化不静默重算通道；稳定 ID 保持旧引用 |

手动创建完成的合法标记可记为 Accepted，确认主体为人工；导入标签与算法结果默认 Pending，可批量确认。原始标签是“数据集给出的标签”，不因导入就宣称它与真实谱完全一致。手动对象的 origin 是创建时几何，模型分数为空，不能填 1 或 0 冒充算法输出。

数值单位固定：时间主键为文件内零起始 uint64 样本索引 `[start,end)`，秒仅作视图；频率保存 Hz，并带 `RF / Baseband / RealOneSided` 坐标声明。JSON 对新增 64 位索引用十进制字符串，禁止经过 double 丢精度；旧版数值按既有安全范围校验后迁移。频率、分数及能量须有限数；缺失值为 null/明确未知，不能伪造零。

时间语义区分 SampleLabel、RoundedLabel、AlgorithmEstimated、Manual、WindowObservation、Unknown；频带区分 NominalLabel、ModelEstimated、Refined、Manual。模型只证明窗口内出现时，将窗口保存在 observation，不写进确定活动段；图上不显示“精确起止”。

Draft 4 按 FFSCN_TEST_GUI 能力补充算法证据：平均/最大模型分数分别保存；唯一窗口命中数与覆盖分母可查；定位方法区分粗帧、sample、ROI，处理状态区分 applied/fallback/skipped。关联父块拆成多个突发时保留 parentCandidate/segment 关系。人工修订不覆盖这些算法值，且窗口命中率不显示为信号占空比。详见 [08](08_FFSCN_TEST_GUI_CAPABILITIES.md) 与 [07 原型证据卡](07_PROTOTYPE_AND_INTERACTION_REVIEW.md)。

## 2. 原始标签适配范围（仅两类）

### 2.1 broadband_signal_generation 的 TXT

依据 `D:/coding/FFSCN_train2/broadband_signal_generation/write_test_record.m` 与已读取的 60 MHz/10 ms 样例。首期覆盖该生成器已核对的带键 TXT 方言及其 10 MHz/20 ms 同源输出；不扩展到 HY 项目的另一种 TXT、HY JSON、YOLO 框或任意 JSON。

| 原始字段 | 归一化与保留规则 |
|---|---|
| 文件头 time_start/time_end/sig_len | 校验记录覆盖范围和完整样本数；与已绑定信号 Fs/长度核对，不按文件名偷偷推断 |
| 行号及 mod/sys | 原始行号作为来源定位；类别字符串原样保留，显示别名另存，不能写成模型识别结论 |
| FC / BW | FC 为生成频率坐标 `[0,Fs)`；转有符号基带 `FC-Fs/2`，已知 RF 中心时再加一次 Fc；边界为中心±BW/2 |
| TIME 多对秒数 | 每行一个 Annotation，每对一个活动段；按采样率映射样本，保留原始文本与舍入信息 |
| CNR / SNR / FD / SPS / LEN | 保存来源属性及原定义，不与算法估计 CNR 混用；LEN 与文件头核对 |

解析按键名而不是固定 token 位置，允许已核对的空白差异；重复关键键、非数值、坏 TIME、越界或缺必需字段逐项报错，不静默跳行。导入默认整批校验成功才发布；用户可明确选择“仅导入有效记录”，但报告必须保存排除行及原因。

writer 将时间写为小数点后 5 位，频率以整数写出。因此文本不保证样本级真值：首期按最近样本取整，保留 10 µs 文本量化步长与至多半步的舍入不确定度，再叠加样本取整误差；没有更精确来源时不可据此宣称 1 sample 级定位误差。BW 是生成参数，不直接等于有限记录里的实测占用带宽。

生成器配套数据是双通道 WAV，当前 Signal Studio 导入器主要支持 RAW/SigMF，不能在规划中把 WAV 描述为已经支持。首期任务包含**受限 WAV 输入适配**：仅支持经夹具核验的双通道 PCM16、I/Q 顺序和 Fs，解析 RIFF chunk、跳过非数据块、拒绝截断和不支持编码，映射到现有 reader 元信息；不得统一假设 44 字节头。这样用户可直接打开生成器配对样本。广义音频格式/压缩 WAV 不在本期。若未来选择预转换 RAW，必须另存逐样本一致性及转换来源，不改变标签时间原点。

### 2.2 TorchSig 指定数据集的 SigMF

规范依据为 [SigMF 官方规范](https://sigmf.org/)，TorchSig 项目用于来源背景；实际适配契约以用户给出的文件为准。该样本自述 `rfbench-v1` 与 `TorchSig 0.5.3.1 synthetic waveform classes`，`rfbench:*` 是这批数据的扩展字段，不能宣称是所有 TorchSig 版本的统一输出。未找到对应生成脚本提交，因此不把当前上游 main 等同于该数据集生成版本。

已读取并计算 SHA-256：

| 文件 | 本次核对结果 |
|---|---|
| `W:/wbSig53_rfbench20m/train/baseline/0000000.sigmf-data` | 3,200,000 bytes；与 cf32_le、400,000 个复样本完全匹配。本次未做模型推理 |
| 同名 `.sigmf-meta` | Fs=20,000,000 Hz，Fc=2,500,000,000 Hz，20 ms，单 capture 从 0 起，4 条 annotations |
| 同名 `.json` | schema=rfbench-v1，4 events，时间为样本半开区间，频率为相对 Fc 的 Hz；包含 nominal bandwidth、emitter/event ID、SNR 定义等 |

导入 `.sigmf-meta` 或 `.sigmf-data` 时只生成一组标记；同名 `.json` 是这一个来源集合的可选补充，不再导入第二组框。`.sigmf-meta` 的采样率、capture 中心、数据格式及 annotations 几何为主；`.json` 用于一致性校验及扩展语义。仅选 `.json` 时查找同名 meta/data 后进入同一路径，不提供独立泛用 JSON 导入器。

| SigMF 字段 | 规则 |
|---|---|
| core:sample_start / core:sample_count | start 直接使用，end=start+count，检查溢出、整数及物理文件边界 |
| core:freq_lower_edge / core:freq_upper_edge | 本样本已是 RF Hz，**不再加 Fc**；绘制基带轴时才减 Fc |
| core:label | 保留 64qam、2gmsk、bpsk 等原字符串，类别映射可选且不改变原值 |
| rfbench:emitter_id / rfbench:hop_index | 保留用于分组与跳频关系；emitter 相同不自动合成长矩形 |
| rfbench:snr_inband_db | 保存值与来源 SNR 定义；不当作模型置信度或界面通用 CNR |
| .json events | 验证样本起止、类别、频边与 meta 一致，再补 event_id、nominal_bandwidth_hz、SNR/轴定义等 |

不能依赖两个数组的顺序配对；用样本区间、频带、类别和可用 emitter ID 建立唯一对应，歧义/冲突显示诊断并阻止自动补充。JSON 缺失不阻止有效 meta 导入；存在但冲突时不得静默覆盖，用户可以明确只导入 meta，保留冲突报告。首期频率核验建议容差 `max(1e-6 Hz, 4 ULP@RF)`，不是 1 kHz 级宽容匹配。

本次四条起止和类别完全一致，RF 减 Fc 后与 JSON 偏移边界的最大差小于 `2.4e-7 Hz`。第 0 条为 `[51107,343277)`、292170 samples，即 `[2.55535,17.16385)` ms；RF 边界 `[2502021605.9327126,2502282009.1227803]` Hz。该例作为未来导入 golden，须恰好产生 4 条事件标记，无双倍频移、无重复条目。

首期针对已核对的零偏移、单通道复数、固定 Fs/Fc、完整时频 annotations。规范允许更多情况，但不自动扩大适配范围：缺 sample_count、缺频率边界、变频 capture、非零外部 offset、多通道歧义和未知必需扩展，标为本 profile 暂不支持而非猜值。可识别的可选扩展原样保存。当前 SigMF 的基本数据导入能力保持；新增标签解释仅覆盖这两类来源中的第二类。

## 3. 导入与来源管理

流程为选择标签/识别配对 → 选择目标文件与样本通道 → 显示坐标、记录/活动段数和诊断 → 提交一个原子导入命令。显式打开 SigMF 时可预选“导入 annotations”，但仍显示数量与解释，不能读取标签就自动创建 DDC。

来源身份包含目标文件格式/通道身份、标签内容摘要、parserId/version、坐标解释及配对数据摘要。相同输入重复导入返回已有集合或允许用户显式另建副本；不依靠文件名去重。源标签更新时建立新版本并展示差异，不覆盖人工修订。重新绑定必须校验数据一致性，修改 Fs/Fc/格式后旧解释标记为需复核，不能悄悄重新投影。

合法标签可以超过当前已加载前缀，但不得超过物理样本长度；超过前缀部分显示“数据尚不可用”，保留原标签。有效分析带宽之外、物理 Nyquist 以内的标签可保留并显示范围外状态；真正超出物理频域的记录报错。显示裁剪和检测 ROI 裁剪都不能修改原始标签。

保留原始标签文件快照、解析结果、未知字段及导入报告于工程内，不修改外部源文件。路径缺失时仍能查看已保存标记与出处，依赖原始 IQ 的证据重算则显示不可用。

## 4. 编辑、显示与通道

原始标签默认锁定，可创建修订；手动来源默认可编辑；算法来源可复核或修订。统一使用样本/Hz 精度的数值编辑与图形控制点，提交一次形成一个命令，Esc 取消。背景刷新不得覆盖正在编辑的草稿。

三来源独立显隐、透明度、锁定；复核状态另作过滤。原始/当前边界仅在选中或“比较修订”时叠画。原始标签可用实线、算法待复核用虚线、被拒绝用灰色与图标；最终颜色遵从现有主题并用图例核验。标签多活动段在表中父项摘要、子项逐段；图上逐段投影，组连接不画成信号区域。

修改来源标记不产生新来源类型；恢复原始值、撤回接受、删除当前引用都是可追溯动作。软删除不改源文件。批量命令预校验所有目标，原子提交，撤销不会重复生成 IDs。跨来源自动合并后置，人工创建派生对象明确记录 derivedFrom。

现有通道依赖 sourceMarkId；迁移后这个旧 ID 解析到同 ID Annotation。新增引用记录 segment/revision，现有通道继续使用创建时参数。删除被通道引用的标记/集合需要沿用并优化现有依赖提示，明确保留来源快照或同时删除哪些通道，默认不级联删除。已确认标记后续修订会显示来源变化，更新通道是显式操作。

## 5. 对照与导出

用户可指定某一来源集合/修订版本为本次对照标签，但“原始标签”不自动成为可靠真值。模型不读取标签作为检测输入；只在评价环节做匹配。标签 nominal BW 与检测 occupied/refined BW 必须在报告中区分，原始真值和人工修订真值分别输出，不能靠改标签提高算法指标。

首期无损导出采用版本化 Signal Studio annotation JSON：包含三来源、活动段、原始值/当前值、修订和出处；CSV 为每活动段一行并注明字段损失。该 JSON 是工程交换格式，不是第三类外部标签适配。回写原生成器 TXT 或 TorchSig SigMF 暂不作为验收范围；以后支持时另存文件并明确精度/扩展损失。

## 6. 边界与规模验收

必须覆盖：两类合法样例、负基带频率、绝对 RF 无二次平移、多突发、多同频/重叠事件、两文件同名、重复导入、JSON/meta 冲突、缺配对文件、非法/非有限数、超界与样本溢出、Partial、源格式变更、撤销/重做、保存重开及通道引用。

旧工程 2000 Mark 上限不能简单移除后全量塞入控件。新集合分片与索引，首期以 100,000 活动段作为压力夹具目标；分页、选择集合、批量命令、取消导入均有预算。目标值是待实施验收门槛，不是当前性能结论。旧工程的合法数据完整迁移，拒绝超预算输入须保留现有工程且给出具体限制。
