# Signal Studio

Signal Studio 是一款运行于 Windows 平台的专业离线数字信号可视化与分析软件，面向大容量 IQ 采样文件的加载、浏览、测量、选区、信号提取、参数估计、调制识别、解调和结果导出。

## 当前阶段

需求基线已生效，项目进入架构与原型设计阶段。当前需求基线为 `SS-SRS-BL-1.0`，归档位置为 [`docs/01_requirements/10_baselines/BL1.0/`](docs/01_requirements/10_baselines/BL1.0/)，对应标签为 `requirements-bl1.0`。

## 文档入口

完整导航见 [`docs/README.md`](docs/README.md)。文档工程覆盖项目治理、需求、产品与 UI/UX、架构、详细设计、接口与格式、验证与确认、项目计划、构建与发布以及用户维护文档。

## 技术栈

需求基线记录的目标技术栈为 Qt 6.11 + Qt Widgets、C++17+、Visual Studio 2022/MSVC x64、CMake，以及可选的 Python/PyTorch、ONNX Runtime 和 TensorRT。

## 目录结构

源代码和构建配置按仓库实际情况维护；工程文档集中在 `docs/`，需求源文档、正式基线和工作追踪矩阵分别位于 `docs/01_requirements/` 的对应子目录。

## 后续工作顺序

先完成 M1 范围内的大文件访问、索引、基础频谱/STFT、渲染、交互、任务和缓存预研，再形成架构/ADR，随后展开详细设计、测试基线和原型评审。

## 需求变更与基线保护

任何需求语义、优先级或验收口径变化必须提交 CR，并完成影响分析、评审、实施和验证。不得直接修改 BL1.0，不得用原型或代码绕过审批；活动矩阵可更新，基线快照不可更新。

## Git 标签

`requirements-bl1.0` 标记正式需求基线提交。后续基线应创建新的明确标签，不覆盖既有标签。
