# Signal Studio

Signal Studio 是一款运行于 Windows 平台的专业离线数字信号可视化与分析软件，面向大容量 IQ 采样文件的加载、浏览、测量、选区、信号提取、参数估计、调制识别、解调和结果导出。

## 当前阶段

需求基线已生效，项目进入 M1 宽带浏览基础的架构、原型和实现准备阶段。当前需求基线为 `SS-SRS-BL-1.0`，归档位置为 [`docs/baseline/BL1.0/`](docs/baseline/BL1.0/)，对应 Git 标签为 `requirements-bl1.0`。

## 技术栈

需求基线记录的目标技术栈为 Qt 6.11 + Qt Widgets、C++17+、Visual Studio 2022/MSVC x64、CMake，以及可选的 Python/PyTorch、ONNX Runtime 和 TensorRT。

## 文档与开发方式

从 [`docs/README.md`](docs/README.md) 进入当前文档；日常开发以 [`docs/DEVELOPMENT_PLAN.md`](docs/DEVELOPMENT_PLAN.md) 为任务入口，用最小设计、实现、编译、测试和 Git 提交流程推进。架构、UI、测试和关键决策分别维护在 `docs/ARCHITECTURE.md`、`docs/UI_DESIGN.md`、`docs/TEST_PLAN.md` 和 `docs/DECISIONS.md`。

## 构建状态

当前仓库尚未确认源码目录和构建入口，编译状态为待确认；不得将文档规划视为软件功能已完成。

## 需求基线规则

`docs/baseline/BL1.0/` 只读。轻微变更直接同步当前文档并记录 CHANGELOG；一般变更同步架构、UI、测试和 CHANGELOG；重大需求变化实际发生时创建变更记录并形成 BL1.1 或 BL2.0。不得直接修改 BL1.0 或伪造签署、测试和完成证据。

## Codex 开发说明

仓库根目录 [`AGENTS.md`](AGENTS.md) 定义 Codex 的长期开发规则、文档权威顺序和每轮任务流程。
