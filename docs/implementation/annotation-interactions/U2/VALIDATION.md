# U2 验证记录

基线为 U1 `3384e17`，分支 `rebuild/a143-native`。本节点验证三来源
直接编辑、自动修订、草稿、当前段及统一删除，不提前宣称 U3 的质量
菜单、UserRange、工程 v6 或新增真实 CPU DDC 已完成。

## 最终自检

使用既有 `scripts/build.ps1`、CTest preset/runner 和
`scripts/screenshot.ps1 -AnnotationInteractions U2`。未修改 480 秒
UI 终止保护。Debug 首次完整运行的 22 项非 UI 检查全部通过；UI 组
在 476.111 秒内结束，107 项通过、1 项旧断言失败。后续只修改该测试，
22 个非 UI 程序逐个 SHA-256 均未改变，保留其通过结果；完整 UI 组
使用同一 preset、runner 和 480 秒保护重跑，最终 **108/108** 通过，
Qt 耗时 **478.377 秒**，CTest 耗时 **478.66 秒**。最终 Debug 的
23 项检查由上述 22 项及重跑 UI 组组成，没有将首次失败记为通过。

领域标记定向 CTest 通过，含 19 个行为场景。四项新增 Qt 行为及
切文件取消用例全部通过，含初始化/清理为 **7/7**、**20.038 秒**。
原生 **10/10** 通过。
offscreen Qt 用例采用 QWidget 回退；原生捕获采用 Windows QRhi，
不能互相替代。

| 证据 | 入口 |
|---|---|
| 首次完整 Debug 配置/构建/CTest | [22 项通过、旧 UI 断言失败的原始日志](logs/build-ctest-debug-initial.txt) |
| 最终构建及完整 UI | [构建](logs/build-final.txt)、[CTest](logs/ui-ctest-final.txt)、[108/108 详细日志](logs/ui-debug-final.txt)、[JUnit](logs/ui-debug-final.xml)、[逐项计时](logs/ui-debug-final.timing.jsonl) |
| 最终五项定向行为 | [7/7 日志](logs/ui-targeted-final.txt) |
| 领域标记定向 CTest | [通过日志](logs/annotations-targeted-final.txt) |
| 非 UI 结果复用依据 | [22 个程序前后 SHA-256](non-ui-binaries-final.json) |
| 原生十场景 | [报告](native-report.json)、[脚本日志](logs/native-debug.txt) |
| 保存重开内容 | [v5 工程快照](saved-project.json)；引用本机实际 IQ，不包含 IQ 数据副本 |
| 复审与静态检查 | [独立复审](REVIEW.md)、[检查记录](static-checks.json) |

| Qt 定向用例 | 直接覆盖 |
|---|---|
| `annotationDirectTableDraftsAndPublication` | 三来源双击/F2；后台发布保留草稿；Enter/Tab；输入框 Delete；Esc 和实际取消按钮；真实 press→事件处理→release 的树按钮不被重建吞掉 |
| `annotationAtomicRangeAndUnknownFrequency` | 多字段一次校验提交；已采纳多段只改当前段；小数 Hz 保留；未知时间仅频率可编辑；刷新穿插拖拽；一次撤销及原始证据不变 |
| `annotationDeletePreviewPreservesDraftAndChannels` | 取消不提交草稿；对象/段/通道计数；一次批删/撤销；保留后到结果与通道快照；标记 PSD 状态归一化及删除后保存 |
| `annotationFileMenuCancelsWithoutSubmittingDrafts` | 实际 File 菜单及文件对话框取消不提交草稿；保存只记录已提交状态；工程替换取消表格草稿 |

领域回归另覆盖三来源同 ID 自动修订、名称/备注保留质量、未知观察
频率边界、编辑上下文失效、删除最后一段、关联快照和后到结果，
以及恢复原始时保留未改兄弟段确认。质量状态导致旧通道按修订号显示
差异的行为仍属于 U3 待替换部分。

## 原生及原型对照

全部原生场景使用指定第二连接屏 Redmi 27 NU / DISPLAY6，实际
3840×2160、150% DPI、2560×1440 逻辑全屏。范围和删除对话框沿用
既有 Capture 的原生裁剪；报告同时校验父窗口全屏及裁剪的实际 DPI。
没有另造截图器或用主屏超大窗口代替。

| 场景 | U0 对照与断言 |
|---|---|
| 01 导入标签直接图形编辑 | 同 ID、原始来源不变；一次拖动产生一条修订，可一次撤销 |
| 02 算法 F2 数值修改 | 编辑当前第二段中心频率，第一段完整状态不变 |
| 03 名称草稿及 Delete | 输入框只删文字；截图/后台刷新不结束草稿；名称提交不重置质量 |
| 04 完整范围草稿 | 13–15 ms 及频带一次应用，第一段不变；中文取消按钮 |
| 05 未定位时间的频率编辑 | 观察支持不变、没有活动段、两端频率控制点，时间列保持未知 |
| 06 当前段调整及撤销 | 只改第二段边界；一次撤销恢复，第一段不变 |
| 07 批删预览 | 2 个对象、4 个活动段、1 个关联通道，一次确认 |
| 08 来源删除后保留通道 | 软删除两个来源，通道及创建时快照保留，关系为 Missing |
| 09 撤销与后台发布 | 恢复所删对象，不移除删除之后发布的结果 |
| 10 保存重开 | 原始证据逐项不变、修订和未知时间保留，通道快照及后到结果保留 |

对应原生截图：[01](screenshots/01-direct-imported-graph-edit.png)、
[02](screenshots/02-table-f2-algorithm-edit.png)、
[03](screenshots/03-name-draft-delete-edits-text.png)、
[04](screenshots/04-atomic-range-draft.png)、
[05](screenshots/05-unknown-time-frequency-only.png)、
[06](screenshots/06-current-segment-single-command-undo.png)、
[07](screenshots/07-batch-delete-preview.png)、
[08](screenshots/08-source-deleted-channel-snapshot-kept.png)、
[09](screenshots/09-undo-keeps-later-background-results.png)、
[10](screenshots/10-origin-and-edits-preserved-after-reopen.png)。
局部修订依据为 [U0 对照](../U0/M0P_DELTA.md)，未改冻结原型。

实际图谱来自一份 20 MS/s、400000 个 CF32 样本的 SigMF；四条标签
来自实际 meta，手动/算法对象为明确的合成交互夹具。三图均提交
AMD Radeon 880M 的 QRhi D3D11 帧，主图纹理上传 2 次，包含重开后的
上传。这是显示和交互证据。关联通道仅为未经 DDC 的配置及快照夹具，
`cpuDdcVerified`、`modelInferenceVerified` 和
`associatedChannelDdcVerified` 均为 false。

## 失败记录与修复依据

中间记录保留在 `out/tasks/wideband-detection/U2/`，不计入最终通过：

- r1 构建：测试头被 include 排序置于其依赖辅助函数前。用独立 include
  分组明确依赖顺序，没有关闭编译检查。
- r2 领域测试：发布时为隔离可变所有者而深拷贝原始证据，测试却比较
  发布前指针。改为发布后只读指针及完整值双重比较，先核对发布内容。
- r2 Qt：直接编辑及批删用例通过；未知拖拽失败，文件菜单用例挂起。
  精确核对本任务测试进程命令后终止，记录终止身份，未作为超时通过。
- r2 原生：前四场景通过，第五场景复现未知拖拽失败。同步刷新会清空
  未知对象的兼容范围；独立手势草稿修复真实路径，保留原断言重跑。
- 文件菜单保存夹具的来源哈希不合法，触发保存错误模态窗口；夹具改用
  明确合成原文的真实 SHA-256，并加保存预检和有界错误处理，未绕过
  工程校验。原生夹具也采用同样的文本哈希规则，实际 IQ 哈希独立记录。
- r3 Qt：三项通过；批删夹具直接设置 PSD 标志被全局偏好覆盖。改为
  通过真实 `psdScope` 控件启用，再检查删除、保存及撤销。
- 首次完整 Debug：22 项非 UI 通过；UI 107/108 通过。旧
  `fileSwitchCancelsUnfinishedEdit` 要求拖动途中修改业务兼容范围，
  与独立草稿语义冲突。仅修改该测试：用实际控制点像素位移证明预览，
  同时验证范围、修订和命令历史未提前改变，切文件取消后释放不提交。
  不是删除原断言后直接宣称通过；补强后的定向检查和独立复审均通过。

规格与质量复审的修复闭环见 [复审记录](REVIEW.md)。
