# Signal Studio Codex 开发规则

## Codex 任务文档

具体开发任务存放在 `tasks/` 目录。

执行任务时：

1. 先阅读 `AGENTS.md`；
2. 再完整阅读指定任务文档；
3. 严格遵守任务范围、禁止项、验证要求和阻断条件；
4. 不擅自扩大任务范围；
5. 完成后更新任务状态和项目核心文档；
6. 输出任务文档要求的执行报告。


## 1. 项目定位

Signal Studio 是基于 C++、Qt、CMake 的 Windows 离线 IQ 信号可视化分析软件，支持宽带分析、窄带分析和宽窄带联动。

## 2. 技术栈

- C++17 或更高
- Qt 6.11、Qt Widgets
- MSVC 2022 x64、CMake
- Windows 10/11
- Python/PyTorch、ONNX Runtime、TensorRT 作为算法扩展

## 3. 文档权威顺序

1. `docs/baseline/BL1.0/`
2. `docs/DEVELOPMENT_PLAN.md`
3. `docs/ARCHITECTURE.md`
4. `docs/UI_DESIGN.md`
5. `docs/TEST_PLAN.md`
6. `docs/DECISIONS.md`
7. `docs/CHANGELOG.md`

正式基线只读，不得直接修改。

## 4. 开发规则

- 一次只完成一个可验证任务。
- 修改前阅读相关需求和核心文档。
- UI 主线程不得执行耗时计算；后台线程不得直接操作 QWidget。
- 大文件不得完整载入内存；核心逻辑不得堆积在界面类。
- 新功能应有对应测试或明确记录为什么暂不可测。
- 不得伪造测试结果、签署材料、负责人或完成状态。
- 用户可见变化更新 `docs/CHANGELOG.md`。
- 每轮结束更新 `docs/DEVELOPMENT_PLAN.md`。
- 先完成当前里程碑，不无控制扩展范围。

## 5. 每轮任务流程

1. 检查仓库状态。
2. 阅读相关需求。
3. 完成最小设计。
4. 实现代码。
5. 编译和测试。
6. 更新文档。
7. 输出变更摘要并提交 Git。

## 6. 需求变更规则

- 轻微变更（文案、默认值、内部实现、小型交互优化）：直接更新对应文档并记录 CHANGELOG。
- 一般变更（非核心功能、接口调整、文件格式扩展）：同步更新 ARCHITECTURE、UI_DESIGN、TEST_PLAN 和 CHANGELOG。
- 重大变更（改变 P0、实时采集、跨平台、宽窄带核心流程或正式性能指标）：实际发生时创建 `docs/changes/CR-XXX.md`，并形成 BL1.1 或 BL2.0。

## 7. 完成条件

功能完成、编译成功、相关测试通过、无明显回归、文档同步、Git 状态清晰。

