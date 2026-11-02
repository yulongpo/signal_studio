# UX-R4 最终精确 HEAD 独立复审

Decision: REJECTED
Reviewed-HEAD: ecf203f809bccf26278b957e1271edb97797c903

基线：`fbb73dbd5990856dd5a63ff616bd3910893bc2ca`；分支 `rebuild/a143-native`。审查者未参与实现；本文件保持提交外。旧报告见 `review-history/26b9fff-REJECTED.md`，其结论不直接决定本次结果。

## 当前结论

R4-IR-01 的主/补充来源遗漏已修复，但新增全文搜索存在两个确定的功能错误，当前 HEAD 仍不满足本轮分页搜索的实际需求。两项均属于常规文本与小型分页验证范围，和用户明确排除的大型事件验证无关。禁止据本报告推送；修复后须对新精确 HEAD 独立复审。

### R4-IR-02 / P2：同页重复词会使后一个跨页命中无法完整选中

`ui/annotations/annotation_evidence_widget.cpp:576` 用 `rawFields_->toPlainText().contains(needle)` 判断是否需要扩展页尾。它判断的是该页任意位置是否有同词，而非本次 `found` 对应的命中是否完整位于页面内。如果前面已经有一次完整命中，后一次从 65530 开始并跨过 65536 页界，则条件为 false，不扩展页面；随后的 QTextCursor 结束位置超出当前文档，选中结果错误，且不能正确连续查找。

小型输入：约 140k 字符全文，偏移 100 和 65530 各放 `BOUNDARY_NEEDLE`。查找第一处后再找下一处。第二处必须完整显示、选中，而当前代码保留 65536 字符页面。使用本机 Qt 6.11.1 的 QPlainTextEdit/QTextCursor 独立 API 探针确认该越界选择不能返回目标词；日志 `out/tasks/wideband-detection/UX-R4-REVIEW/ecf203f/logs/qt-cursor-api-probe.txt`。这是生产代码静态路径与相同 Qt 控件行为共同证明，未声称已在实际应用中新增完整端到端用例。

修复：按本次命中的实际结束位置与当前页面的真实结束位置决定扩展；不要用 contains 代替位置判断。用小型合成文本覆盖“同页前次命中＋后次跨界命中”、连续查找与回绕，保持完整复制不受分页影响。

### R4-IR-03 / P2：保真的 CRLF 原文偏移不能直接作为 QTextCursor 位置

同一函数 `ui/annotations/annotation_evidence_widget.cpp:566`、`:579` 直接把 `fullText_` 的偏移转换为 QTextCursor 位置，并把控件 selectionEnd 加回原文偏移作为下次查找起点。完整主/补充原文允许 CRLF；QPlainTextEdit 将 CRLF 显示为一个段落分隔符，因此原文与文档位置不一致。例：`prefix\r\nheader\r\nTARGET\r\nsuffix` 的原文 TARGET 偏移为 16，控件内为 14；Qt 6.11.1 探针实际选中 `RGET\u2029s`，不是 TARGET。普通 Windows 文本即可触发，不涉及大型事件。

修复：为保真全文与显示/搜索位置建立正确映射，或维护规范化的搜索/显示缓冲及其到原文的映射；完整复制仍使用原始全文。补常规 CRLF 和混合换行小样本的搜索、连续搜索与原文逐字保全断言。

## 本次已实际完成

- HEAD/分支精确匹配，已跟踪工作树/index 起始干净；保留用户旧 R3 未跟踪材料。
- 延续上轮完整需求阅读与全量 UI 代码审查，重新查看相对基线生产 diff；逐行核对本轮来源全文、元数据、分页、搜索、复制重试及辅助上下文修复。
- 独立 Release UX charts/annotation/layout/popovers：4/4，14.11 s；新 annotation 的 4 个业务函数通过。这些用例没有覆盖上述两个搜索边界。
- 独立 Release D3D11 原生 runner：21 主场景＋详情通过，23.00 s，ordinarySupplementalEntriesVerified=2、largeRfbenchValidationExecuted=false，完整复制 38987 字符；日志/报告在 `out/tasks/wideband-detection/UX-R4-REVIEW/ecf203f/`。现有真实普通样本在两处查看/搜索/复制通过，不证明所有搜索边界通过。
- 独立 Qt 控件探针只使用约 140k 合成文本和几十字符 CRLF 文本，未调用 label_import 测试程序、largeSharedAndRecordEvidence 或大型 RFbench 验证。首次探针日志输出因 GBK 无法编码 U+2029 失败，改为 ASCII JSON 输出后完成；未改生产实现。

本次独立执行的项目验证命令：

```powershell
scripts/build.ps1 -Action Test -Configuration Release -TestRegex '^signal_studio_ux_(annotation|layout|charts|popovers)$'
scripts/verify-ux-r4.ps1 -Configuration Release -OutputDirectory 'D:/project/signal_studio_rebuild/out/tasks/wideband-detection/UX-R4-REVIEW/ecf203f/native-release-gpu'
```

Qt API 探针由本机 `D:/profiles/anaconda/python.exe` 的 PySide6 运行，实际 qVersion=6.11.1，使用 offscreen 的独立 QPlainTextEdit/QTextCursor；没有连接或控制用户应用，没有使用 Computer Use。探针是源审查的辅助复现，不算实际 SignalStudio 原生端到端测试。项目原生证明来自上一条实际 runner。

## 修复关闭项与其余范围评估

| 范围 | 独立判断与证据 |
|---|---|
| R4-IR-01 来源完整性 | 当前 `showOriginEvidence` 明确追加 originalText、supplementalOriginalText、主/补充 hash；使用 encodeSourceSet 保留除两段已单独展示原文以外的所有来源元数据，另附 encodeAnnotation 以保留原始证据、windowIds、evidenceRefs 与修订。数据未被按页切割保存，sharedSourceReference 指向的文本仍可取得。常规真实解析器样本在两种组件模式及原生两入口通过；该项在用户当前范围内关闭。大型共享事件只按此路径静态评估，没有执行。 |
| 分页与复制 | 独立全文 QString 缓冲与视口分离，固定 65536 UTF-16 单元分页，页界遇低代理项时扩展至完整码点；复制直接使用 fullText_。约 140k 用例验证后续页尾查找及全文复制，但未覆盖 R4-IR-02/03，因此不能批准“跨页全文搜索全部正确”。 |
| 重试与上下文 | copyFullText 最多 5 次尝试、100 ms 间隔，以读取比较确认结果；失败有重试文案。setFullText 递增请求序号，旧请求回调先检查序号，QObject 接收者销毁后定时器不再调用。上下文改变会关闭详情并更新全文；辅助页隐藏时显式 clearEvidence，新增关闭/换文件测试通过。没有强制重现系统 Error5 或动态注入重试失败；有限性与取消路径为源码检查，恢复后的真实复制由原生运行证明。 |
| 原有 UI 与联动 | 相对 fbb73db 的全量生产改动仍集中于按钮/侧栏、两处证据组件、工具栏重排、图中悬停和树状态；此前审查的参数草稿、定位/选择区别、多段、删除撤销、通道快照等连接未另改。最终原生 runner 包含相关实际点击断言，当前 Release GPU 运行通过。 |
| 工程兼容与功能边界 | 相对基线没有改 domain/application/infrastructure、DSP、worker、标签解析或工程格式源码。当前 runner 实际保存/重开 .ssproj，创建通道并获得 512 个 CPU DDC 样本。未改模块的 7 组合同沿用历史结果，明确不是本次重跑，也不代表所有模型/工程组合通过。 |
| 构建与 Qt 回归 | 审阅 review-fix 的 Debug/Release 构建及最终测试日志：各 UX 4/4、11 个 UI annotations 业务函数所在组 1/1；annotation 为 4 个业务函数。独立复跑 Release UX 4/4。未独立从干净源码重建，不将现有二进制测试说成干净构建；efd6b09 到 ecf203f 的文件清单确认为证据与文档，无生产代码改动。 |
| 原生矩阵与部署 | 四组最新 report 各 21 主场景＋详情、38987 字符复制、两处补充入口=2，largeRfbenchValidationExecuted=false。核对 review-fix 四份 package 报告均 pass=true、systemOnlyPath=true、默认轴模式验证通过，GPU/软件分别记录；这些为实现方归档证据，本审核只另复跑 Release GPU 原生 runner。 |

四组实际屏幕均为 Redmi 27 NU、连接 1、`\\.\DISPLAY6`、3840×2160 / 2560×1440 / DPR 1.5，含 Windows DisplayConfig monitorDevicePath；GPU 为 D3D11 / RTX 5060 Laptop，有绘制与上传断言，软件为 QPainter。最新归档耗时分别为 Release GPU 22.01 s、Release 软件 21.43 s、Debug GPU 52.38 s、Debug 软件 56.08 s；本审核独立 Release GPU 为 23.00 s。

## 截图与断言覆盖

本轮实际打开并目视最新 `release-gpu/03-narrow-320-long-name.png`、`07-full-evidence-dialog.png`、`15-auxiliary-evidence.png`，以及 `release-software/19-dense-roi-long-file.png`。上轮已实际查看基线 before/default、最终 default、长名称、完整详情、辅助证据、密集 ROI 与大频率元数据场景；相关生产布局在此次修复中保持。原生前后证明与本次更新结合判断，没有把未逐张查看的全部 88 张图说成全部目视通过。

所查看场景中主动作有边界、320 px 侧栏关键字段可见、默认首屏未常驻完整 ID/SHA/JSON，密集 ROI 的几何与控点可辨。新详情原文改为水平滚动和分页，复制按钮可见；单张详情截图本身未展示两处普通来源的全文读回，后者由实际原生断言及源码确认。原生几何断言覆盖窗口屏幕/DPR、关键控件命中与高度、水平边界、label heightForWidth 和碰撞；它们不覆盖字符位置映射，故不抵消搜索缺陷。

## 失败、限制与用户范围修订

- **不执行大型 RFbench** 是用户明确修订后的范围，不是测试失败，也不是要求补跑的发布门槛。本次未运行 label_import 全套、largeSharedAndRecordEvidence、批量模型或其他大型矩阵；以后复审也不得以本报告要求补跑大型事件。常规真实样本用于来源保全，小型合成文本用于分页搜索，二者证据不混淆。
- 原系统剪贴板 Error5 及独立 Win32 探针失败有归档；恢复后 clipboard-after-restore.json 为 OpenClipboard=true/error0，四组原生及本次独立复跑成功。没有把当时主机阻塞当作内容断言失败，也不声称已找到系统根因。
- 旧软件 202.54 s 是两帧完成间隔，原因未定位。后续多次未复现可以支持当前流程可完成，不足以宣称长期软件性能稳定性通过。
- 标签历史 4 个符号链接场景是权限 SKIP，仍未在该主机实测。本轮未改解析器；未重跑 7 组合同，其中包含大型事件的历史测试不算本次执行。
- 原型浏览器仍未实测；没有打开或用其他控制方式绕过已停止的浏览器验证。原型/静态文档/offscreen 测试不替代 Qt 原生结果。
- 交互算法标记、密集标记和 999.999 GHz 元数据是合成夹具；普通 TorchSig 标签由真实解析器导入，但这不是模型推理效果证明。四组报告 modelInferenceVerified=false；TensorRT 效果矩阵、现场 RF、设备输入和 Ubuntu 均不在本次通过声明内。
- 未动态注入剪贴板并发争用、未测试所有合法长文本及窗口组合；未独立重跑 Debug/软件矩阵、完整 CTest 或四次 package 启动。上述未运行项不写成通过。

## 交付与结束核验

仅覆盖提交外本报告，并在被忽略的 `out/tasks/wideband-detection/UX-R4-REVIEW/ecf203f/` 保存日志、独立原生截图和报告；未修改实现或仓库测试，未 commit/push/fetch，未批量计算 SHA。旧 R3 未跟踪材料和历史审核均保留。本轮未启动 Computer Use/Node REPL，会话资源没有新增持有；原生 runner 和 Qt 探针均已退出。

结束核验时间 2026-10-25T14:13:23Z：HEAD 仍为 `ecf203f809bccf26278b957e1271edb97797c903`，分支仍为 `rebuild/a143-native`；已跟踪工作树和 index 的 diff 检查均返回 0。未跟踪内容仅用户旧 R3 材料与本报告。

最终结论：REJECTED。R4-IR-01 关闭；R4-IR-02、R4-IR-03 待修复及小型定向验证后，对新精确 HEAD 再独立复审。

