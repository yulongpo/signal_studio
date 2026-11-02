# Signal Studio UX-R4 本地交付

> 修复版已完成 Qt 回归、四组原生验证和独立启动；最终精确 HEAD 的审核和同步状态
> 以提交外 INDEPENDENT_REVIEW.md / PUBLISH_RECORD.json 为准。
> 首次 REJECTED、修复及剪贴板恢复记录见 [POST_REVIEW_FIX](POST_REVIEW_FIX.md)。

完成范围：宽带标记侧栏、来源证据分层、按钮状态、图谱设置降噪、文字布局和跨视图联动。
不重复 R3，不修改算法、DSP、worker 或 .ssproj/v6 持久化格式。

## 使用

- Release：`out/vs2026-qt611-release_bin/SignalStudio.exe`。
- Debug：`out/vs2026-qt611-debug_bin/SignalStudio.exe`。
- 从图/树/统一标记记录选择对象；侧栏上方定位或创建窄带通道。选中不缩放，定位才改变范围。
- 当前段字段沿用 Enter/Tab/应用内失焦提交，Esc 取消；危险删除需要确认且可撤销。
- “技术详情与原文…”打开非模态窗口；“复制完整证据”保留原文、ID、来源、几何和修订，
  包含主/补充原文和各自 hash；全文可分页查看及跨页搜索，复制不受分页限制。
  低频图谱设置在“更多设置”，保持唯一编辑器。

交互设计见 [UX_SPEC](UX_SPEC.md) 和 [prototype.html](prototype.html)。
原型只作为设计交付，浏览器实测因工具 URL 识别限制未完成，不替代 Qt 功能证据。

## 前后对比

- [改动前默认窗口](evidence/before/01-default.png)
- [改动后默认窗口](evidence/release-gpu/01-default.png)
- [320 px 长名称侧栏](evidence/release-gpu/03-narrow-320-long-name.png)
- [完整证据窗口](evidence/release-gpu/07-full-evidence-dialog.png)
- [密集标记与 ROI](evidence/release-gpu/19-dense-roi-long-file.png)

原生窗口均为实际 Redmi 27 NU / DISPLAY6，3840×2160、150% DPI。
四组共 84 个主窗口场景和 4 个详情窗口归档；21 主窗口场景/配置中最后一例是
高频合成元数据排版，不是实际 RF 效果测试。4 次独立应用启动和全部定向结果见 [VALIDATION](VALIDATION.md)。

## 验证及剩余限制

Debug/Release 定向 CTest 各 12/12，真实原生 GPU/软件四组通过，独立启动四组通过。
标签各配置 4 个符号链接场景因主机权限跳过。一次软件帧输出间隔约 202.54 s，
带步骤日志复查未复现；原因未定位，不宣称软件性能稳定性通过。
本轮没有重新验证现场 RF 精度、TensorRT 效果矩阵或 Ubuntu；既有能力边界不变。
按用户最新要求不执行大型 RFbench 事件验证；普通真实导入的两处查看/复制已在原生程序通过。

## Git 与发布

- `9ba2a35`：R0/R1，原生基线、交互原型及规范。
- `6ce2987`：R2/R3，侧栏、证据和按钮/工具栏实现。
- `d4b1959`：R4/R5，布局/原生联动验收、回归证据、发布校验脚本。
- `26b9fff`：首次本地交付说明；独立审核 REJECTED，未推送。
- `efd6b09`：修复补充来源、分页查看和上下文清理；补充定向测试。
- 本文及最终审查对象以 `git log -4` / `git rev-parse HEAD` 记录，不在提交中嵌入自身身份。

开发与主代理验收完成后，由未参与实现的独立 Review 子代理审查最终精确 HEAD。
`INDEPENDENT_REVIEW.md` 为提交外产物，记录 Decision 和 Reviewed-HEAD；只有明确 APPROVED、
当前 HEAD 相同、最新 origin 祖先关系安全、已跟踪内容干净时，才运行
`scripts/publish-ux-r4.ps1 -Push` 正常推送。最终远端结果记录到提交外 `PUBLISH_RECORD.json`。
禁止强推；远端有未整合的新提交时先安全整合、补验并独立重审。

已有未跟踪 `docs/implementation/ux-r3/promotion-fbb73db/` 保留，不混入 R4 提交。
