# Signal Studio 文档

## 正式需求基线

[`baseline/BL1.0/`](baseline/BL1.0/) 是正式需求基线归档，只读。需求正文、交付清单、正式追踪矩阵和 SHA-256 校验文件均保持原内容。

## 当前工作文档

- [`DEVELOPMENT_PLAN.md`](DEVELOPMENT_PLAN.md)：当前任务、里程碑和下一项 Codex 任务
- [`ARCHITECTURE.md`](ARCHITECTURE.md)：最小架构和待验证技术问题
- [`UI_DESIGN.md`](UI_DESIGN.md)：当前 UI/UX 设计
- [`TEST_PLAN.md`](TEST_PLAN.md)：测试策略和测试清单
- [`DECISIONS.md`](DECISIONS.md)：关键技术决策
- [`CHANGELOG.md`](CHANGELOG.md)：用户可见和重要内部变化

## 资源

- [`assets/architecture/`](assets/architecture/)
- [`assets/prototypes/`](assets/prototypes/)
- [`assets/screenshots/`](assets/screenshots/)

## 历史归档

[`archive/previous_document_structure/`](archive/previous_document_structure/) 保存简化前的文档结构和历史需求输入，仅用于追溯，不再作为当前开发入口。

## 日常维护原则

- 当前任务只更新必要文档。
- 不维护重复信息，不提前创建大量空文档。
- 复杂模块达到实际需要时再拆分到 `docs/design/`。
- 轻微变更直接更新对应文档并记录 CHANGELOG。
- 一般变更同步更新架构、UI、测试和 CHANGELOG。
- 重大变更实际发生时创建 `docs/changes/CR-XXX.md`，并形成新的需求基线。
- 正式基线不得直接修改。
