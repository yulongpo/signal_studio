# SS-DE-RPO · 子代理交接

- **Agent**：E（跟踪、融合、持久结果发布）
- **任务编号**：SS-DE-RPO M1 / 契约第 5 节
- **审查基线及实际执行 HEAD**：`82935ab34ec6126707da152bf46d1f352458ab0c`
- **Worktree 路径**：`D:\project\ss-de-rpo-wt\e`
- **工作分支和 commit**：`fix/ss-de-rpo-e`；说明 `fix(ss-de-rpo): explain durable publication mismatches`。SHA 由该提交产生，见交接回复，不嵌入本文件。
- **文件所有权/实际变更文件**：`application/detection_result_service.cpp` 与本目录六份说明。未改 postprocess、projection、refined publication、detection_task、UI。
- **与主代理冻结接口的兼容情况**：兼容。拒绝前缀不变，比较函数仍在，Completed 条件未改，UX-R6 丢弃语义未改。无新运行时 JSON 字段。

## 1. 已确认根因（带证据分类）

| 问题 | 事实类型 | 路径/行号或日志 | 影响 | 修复 |
|---|---|---|---|---|
| 恢复比较失败只报两条固定中文，看不出数量、id 和字段 | CONFIRMED_CODE | `DetectionResultService::replaceCurrentDetection` | 159/159 一类发布失败无法定位字段 | 前缀后追加数量、source set id、第一处 annotation 与 JSON 字段 |
| 旧日志在窗口到齐后仍出现来源/对象数不一致 | CONFIRMED_LOG（旧记录）；当前是否仍复现未测 | `03_AUDIT_BASELINE.md` 第 4 节 | 不能把旧日志当成现在仍稳定复现 | 不删比较。下次失败带字段路径。durable 证据门闩见 `preserveDurableModelEvidence`（CONFIRMED_CODE） |
| 窗口数到齐不等于 Completed | CONFIRMED_CODE | `DetectionTaskManager::execute` / `finish` | 精修、融合或最终 shard 失败仍是 Failed | 不改 task，不把窗口完成标成 Completed |
| 预览上限丢掉正式轨迹 | CONFIRMED_CODE：当前不会 | `rememberPreview` 只裁 `preview`；`finish` 读 `active`/`sealed` | 正式 `FrequencyTrack` 与预览日志分离 | 不改跟踪逻辑 |
| 比较上限靠静默丢轨迹 | CONFIRMED_CODE：当前是响亮失败 | `association-comparison-limit-exceeded`、`frequency-fusion-comparison-limit-exceeded` | 超限失败整帧 | 不把 `4000000` 改大。bin 索引见设计说明，未实现 |
| 已接受启动丢弃本文件本 IQ 通道正式结果，失败不恢复 | CONFIRMED_CODE | `beginDetection` → `discardCommittedFormalResult` | 与“成功后才替换”不同 | 不改。记录在 `ux-r6-contract.md` |

## 2. 代码变更摘要

只扩展 `replaceCurrentDetection` 的两条拒绝字符串。相等判断仍使用 `encodeSourceSet` 与 `encodeAnnotation`。长字段只报告路径和字节数或短标量。

跟踪器、投影和精修发布没有功能改动。

## 3. 验证

| 用例 ID | 运行环境与命令 | 测试结果 | 日志/证据位置 |
|---|---|---|---|
| R-07 | 未跑。Sample/ROI/Coarse 代码未改 | BLOCKED | 本任务禁止完整 CMake configure |
| R-11 | 未跑 159/363/2044 发布 | BLOCKED | 无 engine 运行。窗口完成 ≠ Completed 为 CONFIRMED_CODE |
| R-12 | 未跑 durable / non-durable 实机 | BLOCKED | `preserveDurableModelEvidence` 条件为 CONFIRMED_CODE，不是 RUNTIME |
| R-13 | 未跑 UI | BLOCKED | UX-R6 行为为 CONFIRMED_CODE，见 `ux-r6-contract.md` |
| R-14 | 未跑 shard/提交故障注入 | BLOCKED | 现有 `faultBoundaries` 断言未重跑；比较前缀仍在 |
| R-16 | 未跑 `tests/detection_postprocess_tests.cpp` | BLOCKED | 跟踪逻辑未改。索引未做，以免打破现有比较计数断言 |
| 诊断字符串 | 未单测编译 | BLOCKED | 格式见 `publication-field-diffs.md` |

没有把未运行的用例写成 PASS。没有 GPU 实测，相关项不是 UNVERIFIED_GPU 冒充 PASS，而是 BLOCKED。

## 4. 关键性能与资源峰值

无新测量。比较失败路径在数量相同且来源已不同时会多编码 annotation，直到第一处差异；成功路径的编码次数与原先逐条比较相同。未测 RSS。

## 5. 向其他代理请求的接口及其处理情况

无必须请求。说明见 `interface-requests.md`。

## 6. 未完成 / 风险 / 下一步

- 未在本 worktree 编译或跑 CTest。
- 未重放 159/159 发布失败，因此不能声称字段诊断已经对上某一次历史日志。
- 频率 bin 索引未做。高密度开集仍可能在 `4000000` 次比较处响亮失败。这是现有安全闸，不是静默丢正式轨迹。
- 主代理集成后可由 G 加前缀/后缀断言，并在已有构建上重跑 `detection_postprocess_tests` 与 publication 事务测试。

## 7. 交接声明

已在自己的 worktree 提交，未修改其它 owner 文件；未用 mock 当作真实硬件；没有修改目标分支以外的正式分支。未推送。
