# Signal Studio 变更记录

## Unreleased

### Added

- 新增 `prototype/m1-interactive/` 离线 M1 Signal Studio 交互原型：RAW IQ 导入向导、Canvas 时域/频谱/STFT、项目属性、任务/日志、图谱交互和模拟错误恢复。

### Changed

### Fixed

### Removed

### Documentation

- 增加六张参考图的逐张分析、界面映射和原型运行说明。

- 创建 Figma 原型文件 [Signal Studio — M1 Prototype](https://www.figma.com/design/uiYB8l0egZDdjiWbIMNEwO)，完成 M1 原型 token、字体样式、阴影样式及页面容器；剩余页面组件受 Starter MCP 调用额度限制待续。
- 记录 Starter 计划最多三个页面的原型结构调整，以及 Figma 不可用 Segoe UI 时使用 Inter 的原型回退策略。
- 完成 M1 开发启动核查。
- 根据真实仓库状态更新源码模块记录。
- 完成 RAW IQ 分块读取最小设计。
- 完成 RAW IQ 测试数据约定和单元测试规划。
- 完成 M1-DATA-001 至 M1-DATA-006 任务拆分。

## BL1.0

- 正式需求基线已生效并归档。
- 独立开发文档入口已简化为开发计划、架构、UI、测试、决策和变更记录。
- 当前进入架构、原型和 M1 开发阶段。

## 维护规则

- 用户可见变化登记在本文件。
- 内部小型重构可选择登记。
- 重大需求变化必须说明是否形成 BL1.1 或 BL2.0。
