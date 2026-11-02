# Signal Studio UX-R3 交付

当前代码实施提交为 `bc22bd57458a9c3b0654aa38169dd9ef07c86ce2`，开发分支 `feature/ux-r3-optimization`，交付目标仅为 `origin/rebuild/a143-native`。2026-10-25 修复后再次 fetch 确认目标仍为 `90be58e452f17b121028efc8b34af9be50a76e6e`，它是本地提交的祖先。独立终审及最终防竞态检查完成前不推送。

## 运行入口

本机部署程序：`out/vs2026-qt611-release_bin/SignalStudio.exe`；Debug 对应 `out/vs2026-qt611-debug_bin/SignalStudio.exe`。直接从部署目录启动，文件菜单新建任意目录下的 `.ssproj`，添加信号后进入图谱；也可用 `SignalStudio.exe --project "<path>.ssproj"` 打开。完整操作见 [MANUAL_VALIDATION.md](MANUAL_VALIDATION.md)。

旧 v6 JSON 使用“导入旧工程 v6”复制转换，v1–v5 仍拒绝。旁置资产与 RAW 源引用的移动、另存及签名校验见 [PROJECT_FORMAT.md](PROJECT_FORMAT.md)。源码 clone 不包含 `out/` 中的数据集、模型、TensorRT DLL 和本机工具链；Windows 构建部署仍使用项目规定的 Qt 6.11.1 EXACT 与 VS 2026。

## 功能与证据

| 节点 | 交付内容 | 证据入口 |
|---|---|---|
| G0 | 当前远端、干净跟踪文件基线、已有能力及原生截图 | [BASELINE_AUDIT.md](BASELINE_AUDIT.md) |
| G1 | 工程身份、标签、轴 A/B/C、窄带五页与联动合同；可点击原型 | [DESIGN.md](DESIGN.md)、[prototype.html](prototype.html) |
| G2 | `.ssproj` 原子保存、资产清单、复制/引用、只读 v6 转换、重定位、dirty 与取消 | [PROJECT_FORMAT.md](PROJECT_FORMAT.md)、46 个 codec 行为用例（含活动保存恢复与保根）、工作流 QtTest |
| G3 | 按真实文字测量并共享轴预算，A 默认、B/C 可选，统一物理命中 | [AXIS_LAYOUT.md](AXIS_LAYOUT.md)、轴矩阵 JSON 与 screenshots |
| G4 | 严格 DATA/TXT 同名发现、显式字段配置、可信生成器/SigMF、去重和来源保全 | [LABEL_DISCOVERY.md](LABEL_DISCOVERY.md)、标签套件及真实样本记录 |
| G5 | 观察/星座/眼图/识别/解调五页、共享同步、视图独立及旧页面迁移 | [NARROWBAND_PAGES.md](NARROWBAND_PAGES.md)、判决观察、真实模板 Voronoi 参考区、I/Q 读数；CI16/CF32 原生各 13 场景 |
| G6 | 保持真实检测 worker、冻结请求、复核/标记/通道及 CPU DDC 闭环 | `evidence/review-fixes/release-detection-native.json`，46/46；含真实模型运行中保存后继续推进/取消且不重存恢复 |
| G7 | Debug/Release 构建和完整回归、实际实屏、独立运行与文件关联 | [VALIDATION.md](VALIDATION.md)；该文逐轮保留失败与后续结果 |

实际验收屏为 Redmi 27 NU、连接 1、`\\.\DISPLAY6`、首选与实际 3840×2160、150%，Qt 全屏 2560×1440。G0 历史连接 2 与本轮连接 1 分别记录；未采用其它尺寸替代。

首次终审前 G0–G7 本地实现与验收记录（独立审核随后拒绝；这是历史记录）：Debug/Release CTest 各 40/40；主 UI 106 和窄带 11 业务行完整覆盖；轴四配置各 24/24、窄带四配置各 18/18，Release 真实检测闭环 46/46；最终部署 12/12，实际文件关联打开/注销通过。Debug 全量对应 `064b509`，Release 对应 `717ab8f`；后续只有启动验收入口变化，`05d80d5` 的 Debug/Release 实际部署进程已复验。历史失败及准确版本范围见 VALIDATION，不把不同时点的证据混称同一次全过。

修复提交 bc22bd5 的 G0/G1/G2/G3/G4/G5/G6/G7 功能与本地验收均 Done；最终发布仍须独立 Review 的精确 HEAD 批准。Debug/Release 完整 CTest 各 40/40（1589.32/295.36 s），主 UI 106、窄带 12 行无遗漏/跳过；codec 46/46；新窄带实屏四组合各 26/26；真实 TensorRT 闭环 46/46，新增运行保存恢复检查通过。旧轴 96 场景对应未改变的轴实现；新窄带 104 场景及逐行日志见 evidence/review-fixes/。最终交付版本元数据对齐 bc22bd5，12/12 实际部署进程复验通过，六个 exe 身份见 evidence/review-fixes/final-binary-identities-bc22bd5.json；归档共 151 项，原 71 项字节不变。原始失败保留在 VALIDATION。

## 已知边界

轴布局增加了横向数据宽度，独立文字带也增加纵向预算；按相同外部尺寸重算，A/B 主图纯数据面积减少 1.708%–2.904%，辅助图减少 27.68%–28.56%；C 主图减少 1.896%–3.116%，辅助图减少 16.327%–17.367%。这是独立文字带的实际面积成本，外部分配高度测试不约束纯绘图区高度；原生报告继续分别记录纯数据 plotRect。不能称为总体面积增大。

用户所指未知 MATLAB DATA/TXT 尚无足以确定编码与字段语义的真实样本/规范；提供严格同名发现和可交互字段映射，未猜测解析。可信生成器实际样本为 10 个对象/65 段，TorchSig 实际样本为 4 个对象。受限 TorchSig profile 不代表任意 SigMF 兼容。

窄带以独立生成器产生的 IQ 文件和实际 CPU DDC/恢复链验收，不代表现场 RF 精度。真实识别模型、真实解调位流、持续采集、设备输入和 Ubuntu 运行仍未部署；合成演示明确隔离。真实 TensorRT 推理与 QRhi 显示证据分别记录；当前模型包的 GPU/TensorRT 适配边界保持原交付约束。

## 本地提交与发布门禁

- `180babd`：冻结基线审计、交互合同及原型。
- `3499cfb`：工程文件、测量轴布局、标签与窄带独立页集成。
- `717ab8f`：TXT 映射复用要求精确路径、IQ 绑定及唯一解释。
- `064b509`：独立眼图的顶点 GPU 启动证据、五页验收显式 Rs 假设，以及可注销的 Windows 文件关联。
- `05d80d5`：启动验收异步状态的生命周期与有界的新覆盖层帧检查。
- `bc22bd5`：修复独立审核三项 Major：活动索引完整性、普通保存保留资产根/模式，以及条件判决/参考区域/IQ 读数；补充存储、实际 UI 和真实模型活动保存恢复验证。
- 文档和证据最终提交以实际 Git 日志为准；其后只允许独立审核报告及同步清单作为外部交付记录。

独立审核由未参与实施的 Review 子代理执行，规范报告为本目录 `INDEPENDENT_REVIEW.md`。报告在最后一次提交后生成，记录精确 `Reviewed-HEAD`，作为本地保留的交付附件；再将报告提交会改变被审核 HEAD，因此不为报告本身追加未经重审的提交。审核、代码对象和实际同步通过 `POST_REVIEW_SYNC.json` 关联，审核未通过时该清单不得声称已同步。

实际远端同步必须由 `scripts/publish-ux-r3.ps1` 校验 `Decision: APPROVED`、当前 HEAD 完全一致、跟踪文件干净、最新 fetch/祖先关系，再以非强制方式推送精确 SHA；同步后核验远端 SHA。独立报告与审核后同步清单保留在本地交付目录，由最终答复给出其实际状态及链接。

自动审批审核曾拒绝清理一个任务内小夹具目录，仅返回 `blocked by policy`。该目录按原样保留，位置见兼容检查报告，不影响源工程及验证结果。

本轮 Release 实屏检测启动反馈 367.1684 ms，未达 200 ms 建议目标；取消可见反馈 123.8201 ms，取消完成 278.1047 ms。十万合成活动段筛选 p95 416.1672 ms、选择 p95 435.4769 ms，均未达 200 ms 建议目标。不能沿用旧 R5 性能通过结论；尚无因果剖析证明单一瓶颈，建议目标不升级为新硬门禁。

修复复验的 Release 实屏启动 172.3765 ms、取消可见 67.3124 ms，本次达到建议值；取消完成 224.0829 ms。十万合成活动段筛选/选择 p95 415.1766/425.5413 ms，仍未达 200 ms。首轮未达快照保留，不将一次改善写成稳定全配置性能通过。

文档冻结时：全部本地实施/验收完成，最终规范复审待独立 Review Agent Boyle（未参与修复）绑定最终文档提交 HEAD 出具结论；尚未同步远端。冻结后的实际批准与推送状态由本地 INDEPENDENT_REVIEW.md 和 POST_REVIEW_SYNC.json 联合记录，最终答复给出准确 SHA、报告和同步链接。此处不预签 APPROVED。
