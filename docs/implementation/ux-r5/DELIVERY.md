# UX-R5 本地交付与发布入口

基于 `rebuild/a143-native` 初始 HEAD `5b2a499048d64290851ad02ecf764e17cf678231`。当前任务包五份原始文件保持。用户追加的“删除三个图层 checkbox，来源 Tab 独占时频图叠加”已经纳入设计和实际实现。

## 交付内容

| 阶段 | 产物 |
|---|---|
| R5-0 | `BASELINE_AUDIT.md`，初始本地/远端状态和范围审计；本地提交 `b0dadba` |
| R5-1 | `DESIGN.md`、`DATA_LIFECYCLE.md`、权限/来源快照/正式检测事务合同；本地提交 `079da8c` |
| R5-2/3 | 三个真实表页及独立筛选；Imported/Algorithm 服务与各 UI 入口只读；Manual 编辑及单/批转换、去重、一次撤销；Tab 唯一叠加，无旧混合页/图层复选框 |
| R5-4/5 | FileId+IQ channel 唯一 slot、完整/零结果原子替换、迟到/失败拒绝、旧来源人工修订迁移、通道创建快照、live 恢复及 Reference/私有 GC 保护；主实现提交 `f680f1c` 和随后本轮集成修复提交 |
| R5-6 | Debug/Release 构建与部署、核心/UX/旧 UI 回归、真实 Qt 原生矩阵、独立启动；`VALIDATION.md` 和 `evidence/` |
| R5-7 | 最终提交后由未参与实现的独立子代理审核；报告和发布记录位于提交外，结论以实际文件为准 |

主要实现位于 `domain/annotation.h`、`domain/project.h`、`application/{annotation_service,detection_result_service,session}.*`、`infrastructure/{annotation_json,annotation_migration,project_store,project_file,detection_store}.*`、`ui/annotations/annotation_panel.*`、`ui/detection/{detection_panel,detection_card}.*`、`app/main_window.*`。CPU DDC、STFT、同步算法和模型 worker 算法未重写。

可执行程序：

- `D:/project/signal_studio_rebuild/out/vs2026-qt611-release_bin/SignalStudio.exe`
- `D:/project/signal_studio_rebuild/out/vs2026-qt611-debug_bin/SignalStudio.exe`

两目录含部署 Qt 插件、VC CRT 和 worker；源码 clone 不包含 `out/` 的二进制、模型和运行产物。项目 `scripts/build.ps1` 使用实际工具链重新构建。模型推理能力及其它 GPU 部署不因本轮 UI/存储测试扩大保证范围。

## 验证与复现

`VALIDATION.md` 逐项映射 P0、原始日志、数据和限制。Debug/Release 各18组相关 CTest 通过（其中旧 UI 每配置20个业务函数）；4组原生各15场景及4次生产程序独立启动通过。全部76张原生截图、完整报告和成功日志见 `evidence/INDEX.json`；本机原始失败尝试、项目/伴随资产保留在 `out/tasks/ux-r5/`。

```powershell
./scripts/build.ps1 -Configuration Debug -Action Build
./scripts/build.ps1 -Configuration Release -Action Build
./scripts/verify-ux-r5.ps1 -Configuration Release -OutputDirectory out/tasks/ux-r5/new-native-gpu
./scripts/verify-ux-r5.ps1 -Configuration Release -SoftwareRenderer -OutputDirectory out/tasks/ux-r5/new-native-software
```

每次原生验证须使用新目录，脚本不覆盖历史证据。Debug 对应替换 Configuration。真实实屏必须匹配当次唯一4K/150%探针；不能用其他屏上的大窗口代替。完整相关 CTest 的实际调用及20个旧 UI 函数清单保留在最终日志中。

## 兼容与限制

`.ssproj` schema1 / 原生 v6 保持，新增字段为兼容可选。已转换 Manual 带完整独立来源快照，删除旧 run/源标签后仍可编辑、保存和重开；旧直接修改的来源先保全人工副本；歧义迁移拒绝且原文件保持。通道创建快照与 DSP 配置保存，退役来源准确标示，不静默重绑。

Reference 追加不可变共享声明后保守禁用该根的回收，可能保留磁盘垃圾；私有 GC 有界、异步且受保存锁和原容器引用约束。源 IQ/TXT/SigMF/模型不进入删除候选。未来显式清理共享资产需要单独设计，不能通过删除来源证据实现。

未执行大型 RFbench（用户明确排除）、现场 RF 效果或模型精度、Ubuntu/持续采集。4条标签符号链接测试因本机权限跳过，未伪称通过。代表性 GUI 时延和内存不等价于大型事件吞吐验收。旧通道的多 IQ 读样本验证以创建快照绑定的样本通道为依据，不将当前宽带通道选择解释为通道重新绑定。

## 精确 HEAD 审核与同步

全部代码、测试、证据和本文先本地提交。随后真正独立、未参与实现或修复的 Review Agent 读取最终 HEAD、需求、差异和原始证据，在 `out/tasks/ux-r5/INDEPENDENT_REVIEW.md` 写明确判决及完整 `Reviewed-HEAD`。本文件不嵌入自身最终 SHA 或预签批准。

只有实际 `Decision: APPROVED`、待推送 HEAD 与其完全相等、跟踪文件干净、最新远端为本地祖先时，执行：

```powershell
./scripts/publish-ux-r5.ps1 -Push
```

脚本再次 fetch、校验精确 HEAD 和祖先关系、正常定向推送，并读取远端 SHA。结果写 `out/tasks/ux-r5/PUBLISH_RECORD.json`。远端变化须安全整合、受影响补验及重新独立审核；禁止强推，禁止主代理代审。发布是否完成由该实际记录与远端 HEAD 共同证明，实施日志不能替代批准。

用户原有未跟踪的 UX-R3 promotion、UX-R4 审核/发布记录及 reference ZIP 保留，不属于本轮提交。
