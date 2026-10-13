# 来源、当前能力与原型偏差

状态：2026-10-13 只读审计，Review Draft 4 已补充指定 FFSCN_TEST_GUI 会话；支持本目录规划，不构成算法验收。

## 1. 来源定位方法

用户给出的 HY、智能频谱仪2、ISA_算法中心重构开发、broadband_signal_generation、HY detection training、智能频谱仪，在当前 Codex 中对应六个项目标签。已通过项目列表核对路径，再读取相应会话。**按用户补充要求，主要提取模型能力，不采纳其他项目的交互流程。** 下表保留真实会话标题。历史实施指令仅作背景，不授权本次执行。

| 用户指定上下文 | 实际会话与项目 | 提炼到本规划的内容 |
|---|---|---|
| **FFSCN_TEST_GUI（本轮指定重点）** | [审查 FFSCN_TEST_GUI](codex://threads/01a052c9-0ce5-7e41-a376-2c4a677ce9ca)，`D:/project/FFSCN_train2/FFSCN_TEST_GUI`；5 个轮次 | 优化后的完整 IQ/时频检测、关联、sample/ROI 精修、突发拆分和二维评价；10 文件/594 真值块效果；不继承外部界面流程 |
| HY | [噪底宽带信号模拟](codex://threads/01a0347d-372c-7361-8ca4-c5279e10d750)，`D:/project/HY` | 真噪底叠加信号；定位、类别、标签质量应分别验证；不能从训练损失下降推断实际召回提升 |
| 智能频谱仪2 | [排查并修复瀑布图末段缺失](codex://threads/01a0d6ba-c745-7583-8154-868c2f465975)，`D:/project/01RadioAI` | 该会话主要是显示/设备修复，无新的模型效果评估；补查 SCN/FFSCN 频谱输入接口及长短累积谱后处理，不能从接口存在推断两个后端均已验证 |
| ISA_算法中心重构开发 | [优化累积谱检测带宽修正](codex://threads/01a0cea8-76b2-7200-915d-f4c5156188f6)、[规划添加FFSCN检测后端](codex://threads/01a0cde8-fb36-7950-99f5-e4135e9a19b3)，`D:/project/SCN_based_PSD_Detection` | 最大谱单支推理、邻带平均累积谱取最小值；FFSCN 最近 10 帧出现性、短谱插值、TRT 精度限制 |
| broadband_signal_generation | [调整宽带信号测试集参数](codex://threads/01a076cf-c7f7-7d33-8af2-1c17d37f60a7)、[审查HY 10MHz生成代码](codex://threads/01a08187-916f-79f3-b36b-2d21ab1e27e1)，`D:/coding/FFSCN_train2/broadband_signal_generation` | 60 MHz/10 ms 测试集与 10 MHz/20 ms 训练验证配置不同；逐段 CNR、随机流隔离、WAV/TXT 对齐、数据质量审计 |
| HY detection training | [修复并完善FFSCN训练代码](codex://threads/01a042bb-fc32-7c70-ae3a-5f199af6347c)、[检查环境并恢复第83轮训练](codex://threads/01a0aa4b-f2a8-7162-9ac6-0b6c44a91714)，`D:/project/FFSCN_train2` | 一维多尺度和二维时间块分别评价；模型/权重/参数快照不可混用；GPU 利用率、吞吐与准确率分别量化 |
| 智能频谱仪 | [梳理大带宽检测数据流](codex://threads/01a09f28-fe3b-7960-ab83-d28b692facf7)，`D:/project/isa` | 长谱滑窗 SCN、重叠结果融合、跟踪/边界稳定的后处理能力；时间字段不等于模型直接预测时间；已有 TensorRT 验证限原工程 |

原六组长会话没有逐条穷尽；本轮指定 GUI 会话已读取工具返回的全部 5 个轮次。没有重新连接训练服务器。会话里的历史构建、效果与 GPU 成功记录仍只是各原项目、当时环境的结果。GUI 的能力/效果单列在 [08](08_FFSCN_TEST_GUI_CAPABILITIES.md)，原型来源到操作/交付的逐项映射单列在 [07](07_PROTOTYPE_AND_INTERACTION_REVIEW.md)。

## 2. 当前项目接入审计

本次开始时工作树干净，分支 `rebuild/a143-native`，HEAD 与原型触发版本完全一致。以下行号对应此 HEAD。

| 证据 | 当前事实 | 本期接入方式 |
|---|---|---|
| [domain/project.h](../../../domain/project.h)；[project.cpp](../../../domain/project.cpp) 第 9–29 行 | 时间为 uint64 半开区间；`availableSamples` 与 `fullRange` 区分 Partial、实数单边与复数 RF 频带 | 检测范围沿用这些边界，另建 Run/Candidate/Event，不让 Mark 承担任务状态 |
| [application/session.h](../../../application/session.h) | 现有 `projectGeneration`、文件/标记/通道与视图历史；没有检测服务 | 新标记命令服务统一三来源；保留 Mark 兼容接口，视图前进/后退不当作标记编辑撤销 |
| [domain/sample_format.h](../../../domain/sample_format.h) | 六种编码、实复、多通道、布局、头尾和归一化 | 沿用一个格式解释源；“可读取”与“模型已验证”分开 |
| [raw_sample_reader.cpp](../../../infrastructure/raw_sample_reader.cpp) 第 72 行起 | 有界读取；解码与磁盘片段各受 32 MiB 限制，含 Planar/多通道约束 | 计算输入块预算时考虑通道 stride、两平面及解码临时区，不只看输出 IQ 点数 |
| [source_loader.cpp](../../../infrastructure/source_loader.cpp) 第 10–13 行 | 当前指纹仅为文件大小与修改时间 | 不可当作内容 SHA-256；检测额外记录所读区间内容摘要与格式/通道身份 |
| [spectral_analysis.cpp](../../../infrastructure/spectral_analysis.cpp) 第 267–303 行 | `analyzeSpectrogram` 使用 8M 分析点预算、可抽帧，短窗可调整观测位置/补零 | 不调用此显示入口生成检测序列；保留底层计算借鉴并建立完整固定网格提供器 |
| [spectral_data.h](../../../domain/spectral_data.h) | 线性谱值、源范围、有效/补零样本、ENBW 信息已有基础 | 增加明确单位、模型张量 grid 与物理 grid 映射；不能一律写 dBm 或 dBFS/Hz |
| [main_window.cpp](../../../app/main_window.cpp) 第 353、469、1169、1325 行 | 分析菜单仍为演示动作；底部三页；已有通道弹窗 | 新 `DetectionUiController` 接入菜单、卡片、结果页和任务页 |
| [plot_widget.h](../../../ui/charts/plot_widget.h)、[plot_widget.cpp](../../../ui/charts/plot_widget.cpp) | 现有物理坐标、手势、覆盖层及 QRhi 绘制 | 新 overlay DTO 和 ROI 工具；模型、I/O、CUDA 不进入绘图对象 |
| [project_store.cpp](../../../infrastructure/project_store.cpp) 第 363–374、611 行 | 写入 v4、读取 v1–v4，使用 QSaveFile | 新增 v5 索引/关联与完整迁移验证；不能把多个 QSaveFile 称为整体事务 |
| [CMakeLists.txt](../../../CMakeLists.txt) | Core 无 Qt 链接，Infrastructure 链接 QtCore，UI 链接 Widgets/GuiPrivate | 新算法 core 不依赖 Qt；原生 reader/JSON adapter 仍可使用 QtCore |

仓库 AGENTS.md 部分能力段落还写 v3/CI16；当前 SampleFormat、project_store 和最近执行报告已是多格式/v4。规划采用实际代码事实，原生验收约束、独立实现和不自动提交规则继续遵守；本轮不改 AGENTS.md。

历史基线验收见 [CODEX_EXECUTION_REPORT.md](../../implementation/import-brand-v2/CODEX_EXECUTION_REPORT.md)：记录 Debug/Release CTest 各 7/7、Qt UI 各 87，以及第二连接屏 4K/150% 的 D3D11 与软件路径。这里仅引用已有报告，本次没有重跑。

## 3. 原型包审计

来源目录：`D:/project/signal_studio_原型设计/SignalStudio_Triggered_Design_b151f143`。`FILE_MANIFEST.json` 的 24 个条目全部通过大小与 SHA-256 校验。已阅读专题 01–08、接口头文件/schema/样例、演示 JS，查看原型截图与本仓库已有原生截图。未重新运行原型 QA。

| 编号 | 具体偏差 | 处理原则 |
|---|---|---|
| A01 | `02/04/05`、SCN manifest 和截图仍描述双支推理 | 使用当前已核对的最大谱语义，清除双分支推理验收项 |
| A02 | `Hann/50%` 不是模型训练契约；RAW 与仪器谱被笼统归为 dB | 独立 profile 固定输入形成方法，单位与参考电平显式记录 |
| A03 | 提出任何情况下 10 个完全不补零 FFT 窗；HY 参考又使用有真实时间槽的边缘补零 | 区分“虚构时间帧”和“真实时间槽的 FFT 边缘补零”，按 profile 记录 mask/有效长度 |
| A04 | 同时写参数 HotApply 和运行快照不可变 | 本期只允许显示/结果筛选即时变化；影响计算的变化一律生成新 run 或子派生 run |
| A05 | state 链混合生命周期与流水线阶段 | 采用相互独立的状态轴，见 03 文档 |
| A06 | 演示 `review()` 会直接改变候选并创建/移除 Mark；`renderOverlay()` 仍为未定位候选绘矩形 | 不继承演示的数据操作；建立不可变候选证据与统一 Annotation 修订，接受不复制框，未定位仅带状显示 |
| A07 | README 宣称暂停/合并/拆分，所读 JS 暴露的是 `run/cancel/review` 等有限模拟入口 | 以正式需求与验收定义为准；暂停/复杂批量编辑不因 README 描述就算已有能力 |
| A08 | ABI 注释提及 host table 大小，而入口只接 major/minor；payload 编码/生命周期未冻结 | 头文件继续视为草案，首期不发布稳定 ABI；不复制为对外契约 |
| A09 | 原型迁移表将插件/远程/窄带泛化与宽带交付并列 | 拆成本期、后续明确边界；前期新增 UI 夹具闭环，尽早发现交互问题 |

## 4. 六组上下文的冲突与取舍

**SCN 分支不是当前本地干净发布版。** 本地参考仓库当前为 `scn_dev`，HEAD `be95399aa17409a45380bdf46f3e42e75bfc647b`，有未提交修改；最大谱单支及 BandwidthRefiner 确实在当前文件中，但不能归因于该 HEAD 本身。关键文件工作树 SHA-256 随本规划保存；未修改、切换或清理该仓库。`ffscn_dev` 通过 `git show` 只读查看，提交为 `649cc247f948d8ef71975c9ae1ed4675e12bb68a`。

**HY 的多个数据集不是同一分布。** `HY/dataset_wideband/report/report.md` 描述真噪底叠加、20 Msps/20 ms，但其总数与目录分项存在需要重查 manifest 的口径差异，因此本规划不引用总样本数作为验收规模。MATLAB 60 MHz/10 ms 批次报告记录过 GMSK 排除要求未满足；10 MHz/20 ms 训练/验证入口的验证报告只证明小批量与功能测试，不证明 24,000 份正式数据已生成。

**FFSCN 的三种证据不能串成同一个结论。** HY epoch-210 的 1000 文件频率评估、FFSCN_TEST_GUI 的时间精修行为、epoch-155 FFSCN-17 TensorRT 合成窗口验证各自独立。默认 GUI 权重/阈值已与历史评估不同；迁移前必须冻结完整快照。

**Draft 4 补正：GUI 不只有时间精修行为，也有完整二维效果证据。** 指定会话报告优化后 10 文件/594 真值块，TF-IoU≥0.5 F1=0.8261，另有 88 块真实样本 GUI 自测。前者是小集效果，后者是链路冒烟，两者都不能等同于 1000 文件频率指标。本轮发现历史临时 selftest 已不存在，当前评价器也与最初审计的匹配口径不同，故不把会话的前后指标直接作算法改进量，详见 08。

**不迁移参考项目的操作方式。** 同频去重、稳定 ID、边界稳定是可评估的后处理能力；60 秒 TTL、告警、白名单、设备循环播放、训练监控方式不成为本产品需求。当前界面方案单独依据本项目与原型设计。

**独立实现继续成立。** 外部仓库均为只读行为和数值参考；不复制其 Qt 业务、Flow/Component/HQSigMF 框架，不建立外部源码编译/运行依赖。权重等资产若进入正式包，要记录独立的模型来源与版本；本次未复制资产。

## 5. 关键文件证据索引

| 来源 | 本次用于规划的文件 |
|---|---|
| SCN | `algorithm/DetectionEngine/DetectionEngine.cpp`、`algorithm/refine/BandwidthRefiner.cpp`、`algorithm/DetectionConfig.h`（当前工作树） |
| FFSCN-17 | `ffscn_dev:algorithm/preprocess/FfscnPreprocessor.cpp`、`models/ffscn_17.manifest.json`、`models/ffscn_17_runtime_validation.json` |
| HY | `D:/project/HY/dataset_wideband/report/report.md`，历史训练会话；标签平台期信息另参考历史记忆，未复跑 |
| 数据生成 | `HY_TRAINVAL_10MHz.md`、`hy_trainval_config.m`、`HY_TRAINVAL_VALIDATION_20260908.md`、`GENERATION_REPORT_20260907.md` |
| HY 评估 | `D:/project/FFSCN_train2/reports/eval1000_timefps1ms_20260908/{REPORT_1000.md,summary_1000.json,CNR_REPORT.md,source/}` |
| HY 预处理 | `docs/STFT_TIME_FPS_1MS.md`、`docs/SPECTRAL_BAND_REFINE.md`、`z_test_whole.py`，以冻结评估 source 为最终参考 |
| GUI 主线能力 | `FFSCN_TEST_GUI/README.md`、`business/{config,detection,tracker,evaluation,result_export}.py`、`algorithm/{stft,nms,refine_roi,refine_sample}.py`、`tests/test_review_fixes.py`；本轮另读本地 epoch_1200 权重身份 |
| 标签格式 | MATLAB `write_test_record.m` 及同源 TXT；用户指定 `W:/wbSig53_rfbench20m/train/baseline/0000000.{json,sigmf-meta,sigmf-data}`；另曾调查 HY JSON/TXT，但按最新指令不列入适配范围 |

标记新增审计：现有 `Mark` 仅含 id/name/range；project_store 将标记数组与选择数组限制为 2000 条，并依赖 Mark ID 校验通道；`sigmf_metadata.cpp` 明确提示 annotations 不转为工程标记。三来源支持需要真实改造领域模型、持久化和操作接口，不能只添加图层开关。

上述文件位置是证据位置，不是拟议运行依赖。关键本地文本和冻结快照的当前哈希见 [source_fingerprints.json](source_fingerprints.json)。模型权重哈希来自历史 manifest/评估报告，本次未重新哈希远端权重，不标为现场验证可用。

## 6. 本轮标签范围更正与样本核对

原始标签范围以用户最后补充为准：仅 `broadband_signal_generation`（补充消息中拼作 `broadban_signal_generation`，上下文对应同一生成项目）和指定 TorchSig SigMF。参考 Git 仓库已更正为 [TorchDSP/torchsig](https://github.com/TorchDSP/torchsig)。上游用于来源核验，实际适配以本次可读样本及 [SigMF 规范](https://sigmf.org/) 为准；不推断 rfbench 扩展是所有 TorchSig 的标准格式。

三个样本文件均可读取。cf32_le 数据 3,200,000 bytes 对应 400,000 复样本；20 Msps、20 ms、2.5 GHz 中心。meta 的四条绝对 RF annotations 与 JSON 的四条相对频率 events 在时间/类别上一致，频率转换最大差小于 2.4e-7 Hz；逐文件 hash 已记录。这里只证明样本结构一致，不证明模型在该样本检测正确。

生成器 TXT 的 `TIME` 写为五位小数，`FC/BW` 写为整数。其标签精度与 TorchSig 样本索引不同；应保留文本量化/名义带宽语义。生成器配套是 WAV，当前 reader 尚无该生成器的专用 RIFF 输入路径，因此规划增加受限双通道 PCM16 WAV 元信息适配，复用现有有界样本读取。
