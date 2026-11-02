# Signal Studio UX-R5：标记来源分离与检测结果替换

> 本目录为 **Codex 输入资料**，预期复制到本地仓库 `reference/ux-r5/`。此任务包不是已执行的代码修改，也不是原生 UI 测试结果。

## 文件入口

| 文件 | 用途 |
|---|---|
| `PLAN.md` | 需求基线、架构约束、功能设计、数据迁移、分阶段实施、风险与决策 |
| `CODEX_MASTER_PROMPT.md` | Codex 主 Agent 一次性执行指令；可复制到 Codex 输入框或让其直接读取 |
| `ACCEPTANCE_MATRIX.md` | 细粒度功能、持久化、GUI、并发、恢复与性能验收矩阵 |
| `INDEPENDENT_REVIEW_GATE.md` | 真正独立子代理审核职责、拒绝/批准门槛与安全发布规定 |

## 来源与代码基线

- 仓库：`https://github.com/yulongpo/signal_studio`
- 目标分支：`rebuild/a143-native`
- 本任务包编制时经 GitHub 读取确认的 HEAD：`5b2a499048d64290851ad02ecf764e17cf678231`（2026-10-26）。**它不是开发过程的永久锁定基线**；Codex 必须在执行前仅进行必要的 Git HEAD/祖先关系检查并安全对齐。
- 本轮重用此前 UX-R3 `.ssproj`/DATA+TXT/TorchSig 与 UX-R4 侧栏和来源证据能力，不对已验证的 DSP、TensorRT、QRhi 主通路做无关改动。

## 核心产品约束

1. 底部取消“统一标记记录”混合页，主业务视图变成：**原始标签 / 人工标记 / 检测结果** 三个 Tab；后台任务与日志保留为单独的运维入口。
2. **原始标签与检测结果只读**：可选择、查看、筛选、定位、导出、查看证据；不可修改标记框或直接删除/质量复核/重命名。
3. **人工标记唯一可编辑**；原始标签和检测结果通过显式的单条/批量“转换为人工标记”产生独立副本。
4. 任何一次新算法结果**完整成功发布**后，以当前 `FileId + IQ 样本通道` 为作用域，**整体替换**旧正式检测结果；零结果也替换为空。原始标签、人工标记及窄带通道不得被误删。
5. 检测运行中暂存结果不得混入正式标记；取消/失败不得发布半成品；ROI 更新是本作用域整批替换，不拼接旧 ROI 外的检测框。
6. 旧检测运行的磁盘资产清理不得破坏 `.ssproj` 事务或来源快照，并应防止历史扫描再次恢复已退役结果。
7. **禁止无意义的大批量 SHA 计算**。保留产品必要的已有完整性验证；Git 只做必需的版本/祖先关系核验。
8. 未取得独立 Review 子代理对最终精确 HEAD 的 **APPROVED** 之前，**禁止向任何远端 push**。

## 本地安装提示

在仓库根目录解压该任务包，得到 `reference/ux-r5/`。已有 `.gitignore` 中的 `/reference/*.md` 仅匹配 `reference` 根目录的 MD；本任务的二级子目录通常不受它影响，但开发时仍应执行一次 `git check-ignore -v reference/ux-r5/PLAN.md` 验证是否可跟踪。**不得为了提交资料而全局取消用户现有忽略规则。**

推荐启动输入：

```text
请完整读取 reference/ux-r5/README.md、PLAN.md、CODEX_MASTER_PROMPT.md、ACCEPTANCE_MATRIX.md、INDEPENDENT_REVIEW_GATE.md。按照 CODEX_MASTER_PROMPT.md 的 R5-0 至 R5-7 阶段一次性完成实现、原生验证与独立子代理审核，审核通过前禁止任何远端同步。严禁无意义批量 SHA 计算。
```
