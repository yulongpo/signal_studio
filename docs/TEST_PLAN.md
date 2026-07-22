# Signal Studio 测试计划

## MS-00 测试矩阵

Debug 与 Release UI 配置各执行 45 个具名 CTest 用例。无 Qt 包测试会在移除 Qt 发现变量后，执行一套嵌套且独立的配置、构建、安装和消费流程。

| 覆盖范围 | 每个配置的具名用例数 |
|---|---:|
| 各模块单元/契约/兼容检查 | 10 |
| Core 版本、错误不变量、枚举边界、模块描述符与 DAG 契约 | 8 |
| 插件 ABI：C 编译/链接、C++ `noexcept`/布局、异常封闭、导出符号/版本拒绝 | 4 |
| 通用确定性非发布基准冒烟 | 1 |
| 十个模块的独立确定性性能冒烟 | 10 |
| 基线、外部/最小数据、公共 API、依赖锁、本机配置 | 6 |
| VS Code 本机预设、任务、F5 构建树一致性 | 1 |
| 同进程 PowerShell 环境与用户预设幂等性 | 1 |
| Windows 盘符/UNC/扩展长度根与去重语义 | 1 |
| 获取、兼容宿主、精确快照三种依赖验证模式 | 1 |
| 已安装包消费者：全部组件和无 Qt 无界面组件 | 2 |
| 合计 | 45 |

十个模块用例分别校验描述符结构、API 版本、目标命名空间和能力契约。每个模块另有独立注册的性能测试，通过该模块实现提供器执行 100,000 次真实调用，并周期性校验契约；非发布保护阈值宽松设为五秒。通用基准另执行 250,000 次能力查找。这些测试属于确定性冒烟/回归保护，不是发布性能结论。精确 DAG 测试覆盖跨模块兼容，安装消费者调用全部可用模块目标。

公共 API 检查扫描全部 `.h` 与 `.hpp`，拒绝 Qt、Eigen、oneMKL、TBB、HDF5、ONNX Runtime、FFTW 以及标准库实现类型。枚举负向测试覆盖未知能力可用性、未知模块及未知依赖模块；Status 传播覆盖第七、八、九次上下文添加，验证上限不抛出、根因保留和最新上下文可见。已提交的预设和 VS Code JSON 会扫描本机绝对路径。PowerShell 环境回归还会在同一进程三次生成被忽略的 `CMakeUserPresets.json`，要求字节完全稳定、文件小于 32 KiB、预设名唯一、PATH/`CMAKE_PREFIX_PATH`/INCLUDE/LIB/LIBPATH 项按大小写不敏感规则唯一，并确保 Qt 根目录只出现一次。

Python 与 PowerShell 依赖校验器逐项比较 BL1.0 的选定名称、版本、SPDX、来源、锁和验证字段，并强制未定义哈希具有显式策略。两个校验器还解析不可变 BL1.0 获取脚本，要求 vcpkg 提交、展开后的 `.tar.gz` URL、SHA-256、大小和离线缓存文件名完全一致。模式回归使用注入清单证明：兼容补丁和不同安装路径可接受，不兼容版本、工具族或架构被拒绝，精确快照模式仍拒绝路径变化。Python 清单用例固定 `PYTHONUTF8=1` 与 `PYTHONIOENCODING=utf-8`，校验器还会把标准输出/错误流重配置为 UTF-8 并使用安全替换策略，保证 Windows 传统代码页下的中文缺失路径不会触发编码异常。

最小数据生成器以排序 JSON、POSIX 路径、UTF-8 无 BOM 和 LF 生成清单；`.gitattributes` 固定 Windows 检出仍为 LF。`--check` 同时检查规范序列化、路径分隔符、文件排序和字节一致性，清单验证还检查 Git 属性，防止只在 Windows 出现的 CRLF 差异。

GitHub Actions 工作流在 Windows 2022 和 Ubuntu 24.04 构建无界面平台及 C SDK 示例。两个 Windows Ninja 配置前均显式初始化或复用 x64-hosted x64 MSVC 环境。Qt Windows 作业安装并校验最低支持版本 Qt 6.10.3 `win64_msvc2022_64`，构建十个模块并运行十个性能冒烟用例；本机验证使用 Qt 6.11.1，不可变 BL1.0 port 版本仍保持原值。所有第三方 Action 固定完整提交哈希。

`scripts/validate-ci-workflow.py` 强制 UTF-8 环境、初始化顺序、`Acquisition`/`CompatibleHost` 步骤顺序、Action 固定、Qt 版本/ABI 与官方可用性证据契约。`scripts/validate-vscode-workflow.py` 静态验证本机预设、任务链和明确 F5 目标，并可在构建后动态验证同一构建树及目标新鲜度。既有 `verification.portable_config` 同时扫描 CMake、安装包模板、本机 Qt 发现脚本、Visualization 与 Workbench 编译期守卫：全部必须使用 6.10.3，并拒绝把本机 6.11 身份重新写成最低版本。质量修复精确提交已由远程运行 `29924612586` 验证，Windows UI、Windows 无界面和 Ubuntu 无界面作业全部成功；最终独立规格与代码质量复审均通过。

CUDA 保持可选。缺失 Toolkit 时必须报告并保持 CPU 可构建；没有 `nvcc` 和真实后端时，不宣称 GPU 数值或性能通过。
MS-00 最终验证已收口：本地 Debug 45/45、Release 45/45，合计 90/90；VS Code 任务树 45/45 与直接 F5 目标通过；GitHub Actions 运行 `29924612586` 的三个精确提交作业通过。MS-00 已关闭，后续测试活动从 MS-01 继续。
