# UI 视觉规范

| 元数据项 | 内容 |
|---|---|
| 文档编号 | SS-UI-DS-001 |
| 文档名称 | UI 视觉规范 |
| 项目名称 | Signal Studio / Signal Platform |
| 文档版本 | V1.0.0 |
| 基线版本 | BL1.0 |
| 状态 | 已批准 |
| 内容类型（meta.contentType） | Conceptual |
| 编制日期 | 2026-07-22 |
| 适用阶段 | UI 实现 |
| 输入来源 | 原型视觉令牌、可访问性审计 |
| 本版变更 | 建立公共 Design System 和品牌隔离 |

## 1. Signal Design System

Design System 分为 Signal Design Tokens、Signal UI Components、Signal Chart Components、Signal Workbench Components 和 Signal Icon System。公共主题不含应用品牌；Signal Studio 仅在应用层注入 Logo、名称和强调色。

## 2. 令牌

| 类别 | 令牌 | 建议值 | 规则 |
|---|---|---|---|
| 背景 | `surface.canvas` | `#08111F` | 图表/工作台主背景 |
| 面板 | `surface.panel` | `#0E1B2D` | Dock 与卡片 |
| 文本 | `text.primary` | `#E5F1FF` | 与背景对比 ≥ 4.5:1 |
| 辅助 | `text.muted` | `#8FA8C2` | 仅辅助信息 |
| 强调 | `accent.cyan` | `#20D3EE` | 当前/交互，不独立表达状态 |
| 危险 | `status.error` | `#FF6B7A` | 同时配文字/图标 |
| 间距 | `space.1..6` | 4/8/12/16/24/32 px | 4 px 基线 |
| 字体 | `font.ui` | Microsoft YaHei UI | 中文优先；等宽数值另用 Cascadia Mono |

## 3. 密度与可访问

正文 12–13 px、辅助 11–12 px；视觉紧凑不降低 28×28 逻辑像素命中区。焦点环 2 px；状态不只依赖颜色；图表提供文本摘要。100%–200% DPI 使用逻辑尺寸和矢量图标。

## 4. 品牌隔离

Signal Studio 品牌资产为应用层专有；公共库、SDK、主题和工作台组件不得包含 `Signal Studio` 字样、SS 图形或默认应用菜单。其他工具使用独立 Logo。

## 参考资料

- 原始材料：`../references/`（交付目录之外，只读输入）
- 平台任务提示词（平台化架构版）

## 未决事项

- 无阻断性未决事项；正文中的建议值和待确认项继续按其原状态追踪，不因文档获批而视为已实施。

## 变更记录

| 版本 | 日期 | 变更 |
|---|---|---|
| V1.0.0 | 2026-07-22 | 建立并自动审核通过平台化开发基线，纳入需求、接口、测试和复用边界。 |
