# Signal Studio 架构

## MS-00 平台边界

仓库是一个 C++20/C11 CMake 包，包含八个无界面库和两个可选 Qt UI 库。应用层在 MS-04 前不属于实现范围。经批准的依赖 DAG 同时由配置期断言和可执行契约测试机械校验。

| 公共目标 | 直接公共依赖 | 构建类别 |
|---|---|---|
| `SignalStudio::Core` | 无 | 无界面 |
| `SignalStudio::Compute` | Core | 无界面 |
| `SignalStudio::Data` | Core | 无界面 |
| `SignalStudio::TaskRuntime` | Compute、Core | 无界面 |
| `SignalStudio::DSP` | Data、Compute、Core | 无界面 |
| `SignalStudio::ModelRuntime` | Data、Compute、TaskRuntime、Core | 无界面 |
| `SignalStudio::Dataset` | Data、TaskRuntime、Core | 无界面 |
| `SignalStudio::PluginSDK` | Data、TaskRuntime、Core | 无界面 |
| `SignalStudio::Visualization` | Data、TaskRuntime、Core | 可选 UI |
| `SignalStudio::Workbench` | Visualization、TaskRuntime、Core | 可选 UI |

`SIGNAL_STUDIO_BUILD_UI=OFF` 时不调用 `find_package(Qt6)`，也不创建两个 UI 目标。启用 UI 时 Qt Core/Widgets 仍保持 `PRIVATE`。公共头文件检查拒绝 Qt、Eigen、oneMKL、TBB、HDF5、ONNX Runtime、FFTW 以及标准库实现类型。

## 公共契约

- Core 使用中性品牌名 `Signal Processing Platform`。
- `ErrorCode::stable_text()` 生成经批准的 `SS-<DOMAIN>-E###`。单一不变量路径约束十个域、四个原因、原因/类别映射、严重级别、重试/恢复规则，以及最多八层且逐层校验的原因链。工厂拒绝无效值，序列化会再次校验且不能输出 `unknown` Status。
- `CapabilityRegistry` 和版本化 `ModuleDescriptor` 建立发现与模块兼容契约。
- `plugin_abi_v1.h` 是 C 兼容边界，包含定宽版本化 POD、64 位不透明句柄、显式调用/导出约定、宿主与插件函数表，以及唯一的 `signal_plugin_query_v1` 入口类型。`SIGNAL_PLUGIN_NOEXCEPT` 在 C++ 中展开为 `noexcept`、在 C 中为空，并覆盖全部回调、查询和校验器签名。C++ 异常边界适配器捕获所有实现异常，并将有返回值回调映射为 `SIGNAL_PLUGIN_RESULT_INTERNAL_FAILURE_V1`；编译期断言和故意抛异常的运行期测试共同约束该契约。C11 示例插件仍可作为 C 编译并导出符号。

数据访问、DSP、调度、渲染、模型和数据集的功能 API 在各自里程碑实现，不在 MS-00 中提前宣称。

## 安装包与本机预设

安装树分别导出 `SignalStudioHeadlessTargets.cmake` 和 `SignalStudioUiTargets.cmake`。`find_package(SignalStudio COMPONENTS Core DSP PluginSDK ...)` 始终只装载无界面导出且不发现 Qt；仅请求 `Visualization` 或 `Workbench` 时才定位 Qt。关闭 UI 构建的安装会如实拒绝 UI 组件请求。

测试分别消费包含全部组件的 UI 安装，以及独立配置、构建、安装并消费的无 Qt 安装；两个消费者分别请求、链接、调用并校验十个和八个可用模块目标。

已提交的预设保持路径中立。初始化/配置发现会写入被忽略的 `CMakeUserPresets.json`：唯一的隐藏 MSVC/Ninja 工具链基预设保存规范化后的完整环境（UI 生成时包含单次 Qt `bin` 与 Qt `CMAKE_PREFIX_PATH`），隐藏 Qt 基预设仅增加短标量 `SIGNAL_STUDIO_QT_ROOT`，各本机别名通过继承复用，避免重复嵌入长环境。所有路径列表按 Windows 大小写不敏感规则规范化去重；Qt/Ninja 受控路径先剔除再单次加入，生成不读取旧用户预设。文件内容确定且通过同目录临时文件原子替换。回归测试在同一进程重复生成三次，并跨 PowerShell 7/Windows PowerShell 5.1 比较，要求字节完全一致、全部路径项唯一且 Qt 根目录仅出现一次。

MSVC、Ninja 和 Qt 的进程环境导入保持幂等。MSVC 导入优先复用已有 `VSCMD_VER` 与 `cl.exe` 的 x64 环境；否则先检查 `VSINSTALLDIR`，再通过 `vswhere.exe` 发现带 x64 C++ 工具的 Visual Studio 2022，最后才使用已知本机后备位置。GitHub Windows 2022 的两个 Ninja 作业在 CMake 前显式初始化 x64-hosted x64 环境。

Qt 契约分为三层：项目 UI 源码与安装包的最低支持版本为 Qt 6.10.3，本机实际验证 kit 为 Qt 6.11.1，不可变 BL1.0 的 vcpkg 选择仍为 qtbase 6.11.1#1/qttools 6.11.1。最低版本来自 CI 对当前私有 Qt 使用的真实编译门禁，不把本机版本误写为源码下限。Visualization 与 Workbench 使用相同编译期守卫；CMake、包配置、本机发现脚本、依赖锁扩展字段和静态回归共同拒绝低于 6.10.3 或重新引入 6.11 最低依赖。

远程运行 `29919175820` 已验证提交 `d41c2748a465c4e843617e0a9444c8f8cc2f5015`：Ubuntu/Windows 无界面作业与 Qt 6.10.3 Windows UI 模块/性能作业全部通过。运行 `29918020386` 的旧守卫失败仍作为对应旧提交的历史证据保留，当前远程验收状态以第三轮成功结果为准。
