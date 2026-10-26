# UX-R5 原始证据索引

`INDEX.json` 记录四个真实 Qt 原生完整运行、四个实际菜单专项及四次生产程序独立启动。每个完整 native 子目录保留15张主场景、1张通道对话框、3张完整证据详情图及未改写的原始 report/launcher/steps/stderr。菜单专项 `review-fix/menus-*/` 每配置3张来源主场景及9张真实弹出菜单截图，不能当作完整旅程或复制/DDC 补测。报告中的原始绝对路径仍指向本机 `out/tasks/ux-r5/diagnostics/`；同名 PNG 已完整复制在本目录。

- `release-gpu/03-tab-exclusive-overlay-select-locate.png`：原始标签 Tab，仅该来源叠加；原始、人工和算法对象仍在工程树，三个图层 checkbox 已移除。
- `release-software/09-long-name-width-320.png`：实际320宽侧栏及长名称，软件回退。
- `debug-gpu/11-ordinary-supplemental-auxiliary.png`：完整补充来源详情、跨全文搜索与实际系统剪贴板复制。
- `*/13-new-roi-publication-replaces.png`、`*/14-durable-zero-publication.png`、`*/15-save-reopen-no-old-run.png`：新结果、零结果及重开状态。
- `review-fix/menus-*/menu-{imported,algorithm,manual}-{global,tree,graph}-menu.png`：各来源实际菜单；两只读来源禁用重命名/删除，人工来源可用。

`logs/` 为构建和各配置12组核心/5组 UX/20个旧 UI 函数原始输出，包括首轮及审核整改后批次；`packages/` 是菜单修复后四次生产程序独立启动报告及进程日志。各批次复用/补测关系、跳过、先前失败和测量边界见上级 `VALIDATION.md` 和 `REVIEW_FIXES.md`；失败原件未删除。

这些是实施与验证证据，不是独立审核报告。最终 Reviewed-HEAD 与批准判决必须由提交外独立 Review 生成；不执行全目录指纹盘点。
