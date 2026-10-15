# U0 验证记录

日期：2026-10-15。分支 `rebuild/a143-native`；本轮起点 `a69a68e5b3b7994ebfada013397e58ae154a41da`。本节点只新增局部交互原型、自检和文档；没有修改原生 Qt/DSP/模型/worker/工程版本代码。

## 执行入口

本机工具：Node `v26.3.0`，项目已配备的 Playwright `1.62.1`，Edge headless `155.0.4283.45`。采用项目已有 `.mjs` + Playwright/Edge 操作及截图方式，无新增原生截屏基础设施。新 U0 页面 DOM 与冻结 M0-P 不同，因此新增场景检查驱动，不改写旧驱动或结果。

```powershell
# 从仓库根目录启动审核页面；也可使用交付时已启动的本机 8785 服务。
& 'D:/profiles/anaconda/python.exe' -m http.server 8785 --bind 127.0.0.1 --directory D:/project/signal_studio_rebuild

# 另一个终端执行自检；中间报告默认写入本节点 out 子目录。
& 'C:/Program Files/nodejs/node.exe' scripts/verify_annotation_interaction_model.mjs
$env:SS_PLAYWRIGHT_MODULE = 'C:/Users/Admin/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright'
& 'C:/Program Files/nodejs/node.exe' scripts/verify_annotation_interaction_prototype.mjs
```

可通过 `SS_U0_URL` 指定服务地址、`SS_U0_OUTPUT` 指定浏览器报告输出。前端仅原生 HTML/CSS/ES modules，无第三方运行依赖。新 JS/HTML/CSS 按两空格、单引号、80 列格式化；格式工具 Prettier `3.6.2` 缓存位于 `out/toolchains/npm-cache/`，不进入产品运行或构建依赖。

## 结果与覆盖

最终状态行为 **61/61 通过**，浏览器 **59/59 通过、0 失败、0 页面异常**。运行日志及十张审核截图归档到本目录。原始结果索引：

控制台日志：[状态行为](logs/model.txt)、[浏览器](logs/browser.txt)。

| 验证 | 最终报告 | 范围 |
|---|---|---|
| 状态/命令行为 | [model-checks.json](model-checks.json) | 61 个边界及行为用例；保存加载、来源证据、ID 级撤销、物理坐标、范围与通道条件 |
| 实际浏览器交互 | [browser-checks.json](browser-checks.json) | 59 项 UI 操作；图/表/树选择、悬停、直接编辑、Ctrl/Shift、分页/排序/过滤、批删、质量、两类通道及重开 |
| 冻结基线 | [m0p-frozen-check.json](m0p-frozen-check.json) | 原 M0-P 22 项文件及证据相对本轮起点的 Git 内容一致 |
| 独立复审 | [REVIEW.md](REVIEW.md) | SPEC_PASS / QUALITY_PASS，发现的问题均闭环 |

直接覆盖三来源、三处入口、当前段和其它段分别高亮、原始来源不变、未知时间不生成段、字段无修改不生成修订、真实点击不被提交重绘吞掉、输入 Delete 不删对象、取消按钮 Enter 不确认、窗口事件中断拖动、后台发布保留草稿和焦点。

大场景为 **268 个合成对象，其中 Algorithm 263 个**，超过原左树 100 条和主表 200 条边界。全选匹配覆盖全部页；过滤及关层剔除隐藏选择；选择远端对象加载左树并跳至对应页；排序按当前段，未知时间置尾。它是小规模 UI 行为夹具，不是大量模型效果数据。

通道检查含 Pending/Rejected、当前段裁剪、无段 UserRange、Partial、空视区交集、来源身份改变/不可读、采样率及保护带错误；保存重开保留两类快照和来源已删除通道。人工时段不写回算法活动段；质量/名称/备注不使绑定范围失效。

原 M0-P 的 4 个文本文件在本轮以前经历过已批准的适配修订，且 Windows checkout 有 CRLF；因此不能套用历史 R4 文件字节 SHA 作为本轮起点。冻结检查使用本轮起点 Git blob 与工作区 Git clean 内容比较，同时记录实际工作区 SHA-256；本节点未修改这些旧文件。新交付输入及截图指纹另见 `artifact-manifest.json`。

## 截图

截图来自实际浏览器 UI 状态，**2560×1440 viewport / DPR 1**；不冒称 4K 第二屏、150% DPI 或 Qt 截图。

| 截图 | 可核对事项 |
|---|---|
| [01-ready.png](screenshots/01-ready.png) | 现有工作区结构、三来源、未知时间底纹、统一选择 |
| [02-multisegment.png](screenshots/02-multisegment.png) | 第 2/2 段与主表、图形控制点联动 |
| [03-direct-revision.png](screenshots/03-direct-revision.png) | 当前段直接范围修改、原始证据及修订 |
| [04-background-draft.png](screenshots/04-background-draft.png) | 后台追加时草稿不覆盖 |
| [05-cross-page.png](screenshots/05-cross-page.png) | 268 匹配全选、第二页仍保持选择 |
| [06-user-range.png](screenshots/06-user-range.png) | 未知时间的人工提取范围，不要求先接受 |
| [07-empty-intersection.png](screenshots/07-empty-intersection.png) | 空交集具体错误，无静默整文件回退 |
| [08-delete-preview.png](screenshots/08-delete-preview.png) | 两对象、四段、关联通道的删除预览 |
| [09-waterfall.png](screenshots/09-waterfall.png) | 换轴后物理范围、选择和当前段保持 |
| [10-reopen.png](screenshots/10-reopen.png) | 重开后保留人工范围及来源已删除快照 |

## 能力边界

本节点没有运行 CMake、CTest、原生截图、GPU 或真实 IQ/DDC 验收，因为 U0 的退出对象是浏览器原型。它们必须在 U1–U4 使用项目既有 `build.ps1`、回归、`screenshot.ps1`、`verify-package.ps1` 和 `gpu_smoke.ps1` 完成；原生截图仍须指定 Redmi 27 NU / DISPLAY6 第二连接屏、3840×2160、150% DPI 全屏。

页面图谱、分数、标记、来源可读状态、DSP 计划和通道都是演示数据。demo schema v6 **不表示原生 v6 兼容、升级备份或原子保存已实现**；目前产品仍为原生 v5。U0 自检不能证明模型检测精度、真实 RF 效果或新增真实 DDC 流程通过。
