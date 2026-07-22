# Signal Studio 全量开发、测试与发布任务
## Codex 一次性目标任务提示词

> 当前开发目录：`signal_studio_dev`
> 已批准开发文档：`../Signal_Studio_开发文档/Signal-Studio-Dev-Docs`
> 测试数据目录：`../test_data`
> 目标 GitHub 仓库：`https://github.com/yulongpo/signal_studio.git`

---

## 1. 总体目标

请在当前空目录 `signal_studio_dev` 中，严格依据已经批准通过的 Signal Studio 项目开发文档、软件原型、UI 规范、架构设计、接口定义、测试方案、测试数据说明、项目计划及发布要求，一次性完成 Signal Studio 的：

1. 项目初始化；
2. 平台化软件架构搭建；
3. 公共基础能力库开发；
4. Qt UI 界面开发；
5. 软件业务逻辑开发；
6. 数据加载与信号处理能力开发；
7. CPU/GPU 计算后端开发；
8. 插件和模型运行能力开发；
9. 单元测试、集成测试、UI 测试、算法测试和性能测试；
10. VS Code 编译、运行和调试工程配置；
11. Windows 安装包和便携包制作；
12. 里程碑文档、测试报告和发布文档编写；
13. Git 提交、远程仓库替换、版本标签和 GitHub Release 发布。

最终必须形成一个：

- 可配置；
- 可编译；
- 可调试；
- 可运行；
- 可交互；
- 可测试；
- 可安装；
- 可发布；
- 可持续扩展；

的正式 Signal Studio 桌面软件工程。

不得只生成静态原型、代码骨架、占位类、伪代码、Mock 界面或无法运行的演示工程。

本任务应连续完成全部开发里程碑。在任务完整结束前，不得停止于需求分析、实施计划、单个模块、单个里程碑或中间报告。

---

## 2. 输入目录与材料约束

### 2.1 当前工作目录

所有新代码、构建配置、测试、资产和开发文档均应写入：

```text
signal_studio_dev/
```

除读取批准文档和测试数据外，不得将主要开发成果写入父目录。

### 2.2 已批准开发文档

开发文档位于：

```text
../Signal_Studio_开发文档/Signal-Studio-Dev-Docs
```

开始开发前，必须完整扫描并读取其中的全部内容，包括：

```text
00_交付说明/
01_需求/
02_原型设计/
03_UI规范/
04_技术设计/
05_接口与数据/
06_测试与验收/
07_项目计划/
08_参考资料/
```

这些文档是本次开发的正式基线。

必须重点读取：

- 软件需求规格说明书；
- 功能清单；
- 非功能需求；
- 需求追踪矩阵；
- 原型设计说明书；
- 交互规格说明书；
- 页面清单；
- 状态与异常场景；
- UI 视觉规范；
- 图表显示规范；
- 时频图色阶规范；
- 软件总体架构设计；
- 模块详细设计；
- 线程与任务调度；
- 大文件与缓存；
- GPU 加速设计；
- 架构决策记录；
- RAW/IQ 数据格式；
- 工程文件格式；
- C++、Python 和插件接口；
- 错误码；
- 测试计划；
- 功能测试用例；
- 性能测试方案；
- 算法验证方案；
- 验收标准；
- 开发里程碑；
- WBS；
- 风险清单；
- 第三方库清单。

禁止：

- 不读取文档直接按经验开发；
- 擅自修改已批准需求；
- 静默删除实现困难的功能；
- 将建议内容误写为已批准需求；
- 覆盖父目录中的批准文档原件。

应将批准文档复制为仓库内只读基线快照：

```text
docs/baseline/Signal-Studio-Dev-Docs/
```

同时生成：

```text
docs/baseline/BASELINE_INFO.md
docs/baseline/baseline-manifest.json
docs/baseline/baseline-sha256.txt
```

记录：

- 来源路径；
- 基线版本；
- 复制时间；
- 文件清单；
- SHA256；
- 是否与原始文件一致。

### 2.3 测试数据

测试数据位于：

```text
../test_data
```

必须先阅读该目录中的数据说明、数据清单、格式描述、参数、生成方式和预期结果。

要求：

- 不修改原始测试数据；
- 不删除原始测试数据；
- 不改变原始目录结构；
- 测试程序通过可配置相对路径访问；
- 小型必要数据可复制到仓库；
- 大型测试文件不得重复提交；
- 对外部数据建立清单和校验值；
- 自动化测试找不到大型外部数据时，应准确标记为“环境缺失而跳过”，不得误报通过；
- 另外生成可提交到仓库的最小测试数据和生成脚本。

建议生成：

```text
test-data/
├─ minimal/
├─ generators/
├─ README.md
├─ manifest.json
├─ expected-results.json
└─ external-test-data.example.json
```

---

## 3. GitHub 仓库接入与安全覆盖

目标远程仓库：

```text
https://github.com/yulongpo/signal_studio.git
```

本次开发完成后，新的完整工程应提交并覆盖该仓库当前内容。

这里的“覆盖”应优先采用：

> 保留 Git 历史，通过正式提交删除旧工程内容并加入新工程，而不是直接破坏远程历史。

### 3.1 初始化检查

开始前必须：

1. 检查当前目录是否为空；
2. 检查是否已有 `.git`；
3. 检查远程地址；
4. 获取目标远程仓库；
5. 自动检测默认分支；
6. 获取全部远程分支和标签；
7. 检查工作区状态；
8. 检查 Git 用户信息；
9. 检查 GitHub 认证和推送权限；
10. 记录原远程最新提交。

不得假设默认分支一定叫 `main` 或 `master`。

### 3.2 原仓库备份

替换原仓库内容前，必须基于远程默认分支创建：

```text
archive/pre-signal-studio-dev-YYYYMMDD-HHMMSS
```

并创建保护标签：

```text
pre-signal-studio-dev-YYYYMMDD-HHMMSS
```

若具备写权限，应将备份分支和标签推送到远程。

最终报告必须记录：

- 原默认分支；
- 原最新提交；
- 备份分支；
- 备份标签；
- 新开发分支；
- 新基线提交；
- 正式发布标签。

### 3.3 开发分支

使用独立开发分支：

```text
codex/full-signal-studio-development
```

每个里程碑至少形成一个可追踪提交，不得将全部工作压缩成一个无法审查的巨型提交。

建议提交类型：

```text
chore
docs
build
feat
fix
test
perf
refactor
release
```

### 3.4 合并和推送

全部测试和发布检查完成后：

1. 拉取远程最新内容；
2. 检查开发期间是否出现他人提交；
3. 解决冲突；
4. 将开发分支合并到默认分支；
5. 通过提交删除旧工程中过时内容；
6. 推送默认分支；
7. 创建正式标签；
8. 推送标签；
9. 创建 GitHub Release；
10. 上传发布资产。

正常情况下禁止强制推送。

仅当普通提交无法完成替换时，才允许：

```bash
git push --force-with-lease
```

必须同时满足：

- 已创建并推送备份分支；
- 已创建并推送备份标签；
- 已重新获取远程；
- 已确认没有覆盖其他人员的新提交；
- 使用 `--force-with-lease`；
- 禁止使用裸 `--force`；
- 最终报告中说明原因。

---

## 4. 默认技术基线

以批准文档为准。文档没有明确规定时，采用：

```text
开发语言：C++20
GUI 框架：Qt 6.11
UI 技术：Qt Widgets
可视化 UI：Qt Designer .ui
构建系统：CMake
Windows 编译器：MSVC 2022
主要平台：Windows 11
开发环境：Visual Studio Code
依赖管理：vcpkg manifest mode
算法扩展：Python / PyTorch / ONNX Runtime
GPU：NVIDIA CUDA，可选并支持 CPU 自动降级
测试框架：GoogleTest
性能基准：Google Benchmark
日志：spdlog
格式化：fmt
JSON：nlohmann/json
C++/Python 绑定：pybind11
```

如批准文档选择了不同方案，必须以批准决策为准。

---

## 5. 第三方依赖自动下载安装

### 5.1 强制要求

开发所需的第三方库若当前环境中不存在，必须在必要时自动完成：

- 检测；
- 下载；
- 安装；
- 配置；
- 版本锁定；
- 许可证归档；
- 校验；
- 构建验证。

不得因为缺少依赖只生成说明而停止开发。

### 5.2 获取优先级

依赖获取优先顺序：

1. 项目批准的依赖管理器；
2. `vcpkg manifest mode`；
3. Conan；
4. CMake `FetchContent`；
5. 官方安装程序；
6. 官方源码发行包；
7. 官方 Git 仓库的固定 Tag 或 Commit。

禁止：

- 从来源不明的网盘、论坛附件或第三方镜像下载二进制；
- 下载无许可证库；
- 使用浮动 `latest` 版本；
- 依赖开发者电脑中偶然存在的绝对路径；
- 将未核查的 DLL 直接提交；
- 为了方便绕过许可证限制；
- 自动安装来历不明的软件。

### 5.3 自动安装范围

必要时自动安装或获取：

- CMake；
- Ninja；
- vcpkg 或 Conan；
- Qt 6.11 对应 MSVC 组件；
- Qt Tools；
- Qt Designer；
- MSVC 2022 Build Tools 必要组件；
- Windows SDK；
- Python；
- CUDA Toolkit；
- cuFFT；
- ONNX Runtime；
- FFTW；
- oneMKL；
- Eigen；
- liquid-dsp；
- oneTBB；
- HDF5；
- pybind11；
- spdlog；
- fmt；
- nlohmann/json；
- toml++；
- GoogleTest；
- Google Benchmark；
- 代码检查和打包工具。

以上只是候选范围，不代表全部必须安装。应根据批准架构和依赖清单安装实际需要的最小集合。

### 5.4 系统环境修改规则

优先使用项目级或用户级安装，避免不可逆的系统级污染。

安装策略：

1. 优先使用 vcpkg、Conan、Python 虚拟环境和项目本地工具目录；
2. 系统级安装仅用于 Qt、MSVC、Windows SDK、CUDA 等确有必要的工具链；
3. 安装前检测现有版本；
4. 已存在兼容版本时复用；
5. 不得降级或覆盖其他项目依赖；
6. 不得卸载用户已有工具；
7. 不得静默修改系统安全策略；
8. 不得关闭证书校验；
9. 不得修改防病毒策略；
10. 不得把密钥和令牌写入脚本。

若某项系统级安装需要管理员权限而当前环境不允许：

- 完成其余全部工作；
- 生成准确的安装脚本；
- 生成无人值守参数；
- 生成安装验证脚本；
- 在报告中标记唯一阻塞项；
- 不得声称已安装成功。

### 5.5 版本和校验

必须生成：

```text
vcpkg.json
vcpkg-configuration.json
```

或批准方案对应文件。

同时生成：

```text
docs/development/第三方依赖安装说明.md
docs/development/第三方依赖版本锁定清单.md
docs/development/第三方许可证清单.md
scripts/bootstrap.*
scripts/install-dependencies.*
scripts/verify-dependencies.*
```

记录：

- 名称；
- 版本；
- 来源；
- 官方 URL；
- 许可证；
- 用途；
- 安装方式；
- 包管理器端口或配方；
- SHA256 或提交哈希；
- CPU/GPU；
- Debug/Release；
- 静态/动态链接；
- 发布包是否包含；
- 商业发布注意事项。

### 5.6 可重复安装

在全新开发环境中，应能够通过一个入口完成依赖准备，例如：

```powershell
.\scripts\bootstrap.ps1
```

或：

```bash
python scripts/bootstrap.py
```

该入口应：

1. 检测操作系统；
2. 检测 Visual Studio；
3. 检测 Qt；
4. 检测 CMake；
5. 检测 Ninja；
6. 检测 CUDA；
7. 检测依赖管理器；
8. 安装缺失的项目级依赖；
9. 配置环境；
10. 验证版本；
11. 输出可读报告；
12. 返回正确退出码。

---

## 6. 平台化架构

Signal Studio 不得实现为封闭单体应用。

必须建设以下公共能力：

```text
SignalCore
SignalData
SignalDSP
SignalCompute
SignalTaskRuntime
SignalVisualization
SignalWorkbench
SignalPluginSDK
SignalModelRuntime
SignalDataset
```

这些能力后续必须能够复用于：

```text
信号仿真生成工具
批量样本生成工具
样本库管理工具
模型训练工具
模型评估工具
模型推理工具
其他信号分析工具
```

推荐目录：

```text
signal_studio_dev/
├─ apps/
│  └─ signal-studio/
├─ libs/
│  ├─ signal-core/
│  ├─ signal-data/
│  ├─ signal-dsp/
│  ├─ signal-compute/
│  ├─ signal-task-runtime/
│  ├─ signal-visualization/
│  ├─ signal-workbench/
│  ├─ signal-plugin-sdk/
│  ├─ signal-model-runtime/
│  └─ signal-dataset/
├─ plugins/
│  ├─ importers/
│  ├─ exporters/
│  ├─ algorithms/
│  ├─ models/
│  └─ visualizations/
├─ tests/
│  ├─ unit/
│  ├─ integration/
│  ├─ ui/
│  ├─ performance/
│  ├─ compatibility/
│  └─ golden/
├─ test-data/
├─ tools/
├─ scripts/
├─ cmake/
├─ packaging/
├─ docs/
│  ├─ baseline/
│  ├─ development/
│  ├─ milestones/
│  ├─ testing/
│  └─ release/
├─ assets/
├─ .github/
├─ .vscode/
├─ CMakeLists.txt
├─ CMakePresets.json
├─ vcpkg.json
├─ README.md
├─ CHANGELOG.md
└─ LICENSES/
```

必须遵守：

- 基础库不得依赖应用层；
- `SignalCore` 不依赖 Qt Widgets；
- `SignalDSP` 不依赖具体页面；
- `SignalData` 不依赖 Signal Studio；
- 应用不得直接调用 FFTW、cuFFT、oneMKL 等具体实现；
- 具体第三方实现通过 Adapter 隔离；
- 插件不得依赖 Signal Studio 页面；
- 公共库不得硬编码 Signal Studio 品牌；
- 禁止循环依赖；
- 公共 API 尽量不暴露第三方类型；
- 每个基础库可独立构建和测试；
- 每个公开库生成 CMake Target 和命名空间 Alias；
- API 和 ABI 具有版本策略。

建议依赖方向：

```text
Applications
    ↓
SignalWorkbench / SignalVisualization / SignalModelRuntime / SignalDataset
    ↓
SignalTaskRuntime / SignalDSP / SignalData
    ↓
SignalCompute / SignalCore
    ↓
Third-party adapters / OS
```

---

## 7. Qt UI 开发

### 7.1 可预览 UI

主要页面和窗口必须生成 Qt Designer 可打开的 `.ui` 文件。

至少包括：

- 主窗口；
- 项目首页；
- 宽带浏览；
- 窄带分析；
- RAW/IQ 导入向导；
- 加载进度窗口；
- Inspector；
- 任务中心；
- 结果中心；
- 插件与模型；
- 设置与诊断；
- 关于页面；
- 错误、警告和确认弹窗。

要求：

- 可在 Qt Designer 中预览；
- 使用布局管理器；
- 禁止大量绝对坐标；
- 支持窗口缩放；
- 支持高 DPI；
- 对应批准原型和 UI 规范；
- 使用已批准 Logo 和图标；
- UI 与业务逻辑解耦；
- 控件对象名统一；
- 主要操作接入真实逻辑；
- 不得将 HTML 原型直接包装为最终 Qt 软件；
- 不得以静态截图冒充界面。

### 7.2 UI 预览成果

生成：

```text
docs/development/ui-preview/
```

包括：

- 主要页面截图；
- 1280×720；
- 1920×1080；
- 高 DPI 检查；
- 页面截图索引；
- Qt Designer 打开说明；
- 应用启动说明；
- UI 自动截图脚本。

### 7.3 图表交互

必须实现批准文档规定的：

- 时间导航；
- 时域波形；
- PSD；
- 功率谱；
- 瀑布图；
- 时频图；
- 星座图；
- 眼图；
- 游标；
- 测量；
- 框选；
- 缩放；
- 平移；
- 视图重置；
- 多图联动；
- 显示和隐藏；
- 高度调整；
- 位置交换。

重点要求：

1. 时间导航范围等于实际读入数据长度；
2. 时间导航窗格高度较小；
3. 可交互区域使用十字光标；
4. 时域和功率谱可以隐藏；
5. 隐藏后停止对应视图专属计算和渲染；
6. 瀑布图支持多种色阶；
7. 瀑布图支持参考电平和动态范围；
8. 功率谱与瀑布图共享频率轴；
9. 功率谱纵轴为 `dB/Hz`；
10. 瀑布图纵轴为时间，默认 `ms`；
11. 瀑布图时间范围受时间导航控制；
12. 滚轮只缩放频率横轴；
13. 右键从左向右选择频率范围；
14. 右键从右向左重置频率范围；
15. 三个谱图高度可调；
16. 三个谱图位置可交换；
17. 频率单位在 Hz、kHz、MHz、GHz 间自适应；
18. 频率显示保留 Hz 级精度。

---

## 8. VS Code 编译、运行和调试

必须提供完整 VS Code 配置，使开发者能够在 Windows 11、MSVC 2022、Qt 6.11 环境中完成：

- CMake 配置；
- Debug 编译；
- Release 编译；
- F5 启动调试；
- 单元测试；
- 集成测试；
- 性能测试；
- 安装包构建。

生成：

```text
.vscode/
├─ extensions.json
├─ settings.json
├─ tasks.json
├─ launch.json
└─ c_cpp_properties.json
```

生成：

```text
CMakePresets.json
CMakeUserPresets.json.example
```

至少包含：

```text
windows-msvc-debug
windows-msvc-release
windows-msvc-relwithdebinfo
windows-msvc-cpu-debug
windows-msvc-cpu-release
windows-msvc-cuda-debug
windows-msvc-cuda-release
```

要求：

- CPU 模式不依赖 CUDA；
- CUDA 不可用时自动禁用；
- 主程序仍可编译运行；
- `launch.json` 可调试主程序；
- 可调试单元测试；
- 可调试指定测试；
- 正确配置 Qt 插件和 DLL 路径；
- 不写入个人机器绝对路径；
- 必须实际验证配置流程。

生成：

```text
docs/development/VSCode构建与调试指南.md
```

---

## 9. 软件功能范围

以批准 SRS 和需求追踪矩阵为准，至少完成以下闭环。

### 9.1 应用工作台

- 主窗口；
- 菜单；
- 工具栏；
- Dock；
- 页面导航；
- Inspector；
- 状态栏；
- 主题；
- 布局保存；
- 快捷键；
- 通知；
- 日志；
- 诊断；
- 错误提示。

### 9.2 数据导入

- RAW/IQ；
- 实数和复数；
- I/Q 交错和分离；
- 不同位宽；
- 整数和浮点；
- 有符号和无符号；
- 大端和小端；
- 采样率；
- 中心频率；
- 数据偏移；
- 读取长度；
- 缩放；
- 参数校验；
- 导入预览；
- 解析进度；
- 暂停；
- 继续；
- 取消；
- 取消后的有效只读前缀；
- 异常恢复。

### 9.3 大文件和缓存

- 分块读取；
- 内存映射或批准方案；
- 多级缓存；
- 数据窗口；
- 多分辨率显示；
- 异步 I/O；
- 内存预算；
- 缓存淘汰；
- 取消；
- 资源释放；
- 工程恢复；
- 不强制全量读入内存。

### 9.4 信号分析

- 时域；
- FFT；
- PSD；
- STFT；
- 瀑布图；
- 时频图；
- 数字下变频；
- 滤波；
- 重采样；
- 星座图；
- 眼图；
- 测量；
- 峰值；
- 游标；
- 宽带分析；
- 窄带分析；
- 宽窄带联动。

### 9.5 任务中心

- 任务提交；
- 队列；
- 优先级；
- 状态；
- 进度；
- 暂停；
- 恢复；
- 取消；
- 错误；
- 日志；
- 结果；
- 历史；
- 资源统计。

### 9.6 结果中心

- 结果查看；
- 结果分类；
- 结果搜索；
- 图形导出；
- 数据导出；
- 分析报告；
- 状态追踪；
- 错误信息。

### 9.7 插件和模型

- 插件发现；
- 插件加载；
- 插件卸载；
- 能力声明；
- 版本检查；
- 错误隔离；
- 模型注册；
- ONNX Runtime 或批准方案；
- CPU/GPU 选择；
- 推理接口；
- 模型元数据；
- 兼容性检查；
- 示例插件和示例模型适配器。

### 9.8 工程管理

- 新建；
- 打开；
- 保存；
- 另存为；
- 相对路径；
- 自动恢复；
- 版本迁移；
- 数据源引用；
- 视图状态保存；
- 插件和模型配置保存；
- 损坏检测；
- 兼容性提示。

批准文档中明确属于后续版本的算法，可以实现完整插件接口、测试桩和明确的“未安装算法”状态，但禁止展示伪造算法结果。

---

## 10. 测试

### 10.1 单元测试

覆盖：

- SignalCore；
- SignalData；
- RAW/IQ Parser；
- 字节序；
- I/Q 布局；
- 单位格式；
- 频率精度；
- 错误模型；
- 任务状态机；
- FFT Adapter；
- PSD；
- STFT；
- 窗函数；
- 滤波；
- 重采样；
- 缓存；
- 工程文件；
- 插件元数据；
- 模型元数据。

### 10.2 集成测试

覆盖：

- 文件导入到可视化；
- 导入任务到任务中心；
- 时间导航到谱图；
- 功率谱和瀑布图联动；
- 隐藏视图后的计算停止；
- 宽带选区到窄带处理；
- 工程保存和恢复；
- 插件加载；
- 模型推理；
- CPU/GPU 切换；
- 异常恢复。

### 10.3 UI 测试

覆盖：

- 页面打开；
- 导入向导；
- 加载进度；
- 暂停；
- 继续；
- 取消；
- 视图显示隐藏；
- 图表高度调整；
- 图表顺序交换；
- 频率框选；
- 反向重置；
- 设置保存；
- 错误弹窗；
- 高 DPI。

### 10.4 算法正确性

使用 `../test_data` 和批准测试方案验证：

- FFT 频率；
- FFT 幅值；
- PSD；
- `dB/Hz`；
- 窗函数增益；
- STFT 时间轴；
- STFT 频率轴；
- 单音；
- 双音；
- 线性调频；
- 噪声；
- 宽带多信号；
- CPU/GPU 一致性；
- MATLAB 或批准参考结果。

不得只凭视觉判断。

### 10.5 性能

至少测量：

- 启动时间；
- 文件读取吞吐；
- 首屏显示时间；
- 内存峰值；
- FFT 性能；
- STFT 性能；
- 缓存命中；
- 图表缩放延迟；
- 图表平移延迟；
- UI 帧率；
- 任务取消响应；
- CPU/GPU 加速比；
- 隐藏视图后的资源变化；
- 长时间运行稳定性；
- 资源释放。

### 10.6 测试报告

生成：

```text
docs/testing/
├─ 测试环境.md
├─ 单元测试报告.md
├─ 集成测试报告.md
├─ UI测试报告.md
├─ 算法正确性测试报告.md
├─ 性能测试报告.md
├─ 稳定性测试报告.md
├─ 缺陷清单.md
└─ 测试总结.md
```

测试失败必须修复后重跑。

禁止：

- 删除失败测试；
- 注释掉测试；
- 降低容差掩盖问题；
- 伪造测试结果；
- 将“未运行”写成“通过”。

---

## 11. 里程碑

依据批准里程碑执行。若批准文档没有更细定义，采用：

```text
M0 文档、仓库、工具链和构建基线
M1 SignalCore、SignalData、SignalTaskRuntime
M2 SignalDSP、SignalCompute
M3 SignalVisualization、SignalWorkbench
M4 数据导入、时间导航和基础谱图
M5 宽带、窄带和联动分析
M6 任务、结果、插件和模型
M7 性能、稳定性、工程和安装
Beta 全量测试与验收准备
Release 正式发布
```

每个里程碑在：

```text
docs/milestones/
```

生成：

```text
MS-XX_计划.md
MS-XX_实施记录.md
MS-XX_测试报告.md
MS-XX_完成报告.md
```

每个完成报告至少记录：

- 目标；
- 完成功能；
- 修改文件；
- 架构变化；
- 依赖变化；
- 测试结果；
- 性能结果；
- 已知问题；
- 需求覆盖率；
- 下一里程碑输入；
- Git 提交哈希。

本任务连续完成全部里程碑，不等待人工逐项批准。

---

## 12. 代码质量

配置并执行：

- 编译器高警告级别；
- `clang-format`；
- `clang-tidy`；
- 静态分析；
- Include 依赖检查；
- 资源泄漏检查；
- Sanitizer 或 Windows 等价工具；
- 第三方许可证检查；
- 重复代码检查；
- API 文档生成。

要求：

- 公共接口有统一注释；
- 复杂算法说明来源；
- 不保留无说明 TODO；
- 不保留死代码；
- 不提交构建缓存；
- 不提交密钥；
- `.gitignore` 完整；
- 公共接口有测试；
- 避免全局可变状态；
- UI 主线程不执行耗时计算；
- 资源所有权明确；
- 错误处理统一。

---

## 13. CI/CD

生成 GitHub Actions：

```text
.github/workflows/
├─ build-windows.yml
├─ test.yml
├─ code-quality.yml
└─ release.yml
```

至少包括：

- Windows MSVC；
- 依赖缓存；
- Debug 编译；
- Release 编译；
- CPU 模式；
- 单元测试；
- 集成测试；
- 静态检查；
- 格式检查；
- 构建产物；
- 安装包；
- Release 上传。

CUDA CI 不可用时：

- CPU 构建必须为强制检查；
- CUDA 可设置为可选；
- 不得阻塞 CPU 发布；
- GPU 测试由专用 Runner 或本地报告完成。

---

## 14. 安装和发布

### 14.1 Windows 安装包

生成：

- 安装包；
- 便携包；
- Qt Runtime；
- 第三方 DLL；
- 插件目录；
- 模型目录；
- 配置目录；
- 示例数据；
- 许可证；
- README；
- 版本信息。

采用成熟方案：

- CPack；
- Qt deployment tools；
- 批准文档指定安装器。

必须验证：

- 安装；
- 启动；
- 卸载；
- 升级；
- 便携运行；
- 无缺失 DLL；
- 无开发机绝对路径；
- 不依赖 Visual Studio 开发环境。

### 14.2 版本

以批准策略为准。若未明确，使用：

```text
Signal Studio 1.0.0
公共基础库与 SDK 1.0.0
```

生成：

```text
CHANGELOG.md
docs/release/ReleaseNotes_1.0.0.md
docs/release/发布检查清单.md
docs/release/安装验证报告.md
```

### 14.3 GitHub Release

完成后：

1. 合并默认分支；
2. 创建版本标签；
3. 推送标签；
4. 创建 GitHub Release；
5. 上传安装包；
6. 上传便携包；
7. 上传 SHA256；
8. 上传发布说明；
9. 验证远程发布页面；
10. 记录 Release URL。

若缺少 GitHub Release 权限：

- 完成代码推送；
- 完成标签；
- 生成全部发布资产；
- 生成 SHA256；
- 生成可直接运行的发布命令；
- 如实记录阻塞；
- 不得声称已发布。

---

## 15. 需求追踪

维护：

```text
docs/development/开发需求追踪矩阵.xlsx
```

每项需求关联：

```text
需求编号
所属应用或基础库
源码模块
页面
接口
测试用例
里程碑
Git 提交
状态
验收结果
```

完成前保证：

- 所有 P0/P1 需求有实现；
- 所有 P0/P1 需求有测试；
- 所有 P0/P1 需求有验收结果；
- 后续版本功能有明确状态；
- 不存在悬空需求；
- 不存在无需求来源的核心功能。

---

## 16. 自主执行规则

### 16.1 不反复询问

文档足以确定时直接执行。

### 16.2 不得提前结束

以下不算完成：

- 只给计划；
- 只创建目录；
- 只完成 UI；
- 只完成一个里程碑；
- 只完成 Debug；
- 只完成 CPU；
- 只运行部分测试；
- 只提交本地 Git；
- 只输出后续建议。

### 16.3 阻塞处理

遇到环境、权限、网络、硬件或许可证阻塞：

1. 记录真实错误；
2. 完成所有不受影响的工作；
3. 自动尝试官方替代安装方式；
4. 实现可用降级路径；
5. 生成复现命令；
6. 生成解决脚本；
7. 如实记录未完成项；
8. 不伪造成功；
9. 不因单一阻塞停止整个任务。

### 16.4 禁止破坏性行为

禁止：

- 删除批准文档；
- 删除原始测试数据；
- 无备份覆盖远程历史；
- 使用裸 `git push --force`；
- 提交凭据；
- 覆盖他人新提交；
- 禁用失败测试；
- 用静态截图冒充程序；
- 使用无许可证依赖；
- 修改系统安全设置以完成安装。

---

## 17. 最终交付物

仓库至少包含：

```text
完整 C++/Qt 源码
公共基础能力库
Signal Studio 应用
Qt Designer .ui
CMake 工程
CMake Presets
依赖管理文件
依赖自动安装脚本
VS Code 配置
单元测试
集成测试
UI 测试
算法测试
性能测试
最小测试数据
测试数据生成脚本
插件示例
模型接口示例
基线文档快照
开发文档
里程碑文档
测试报告
UI 预览截图
安装包
便携包
许可证
发布说明
CHANGELOG
GitHub Actions
最终执行报告
```

---

## 18. 最终验收条件

只有同时满足以下条件，任务才能判定完成：

- [ ] 已读取全部批准文档；
- [ ] 已读取测试数据说明；
- [ ] 已保存文档基线快照；
- [ ] 已接入目标 GitHub 仓库；
- [ ] 已备份原远程默认分支；
- [ ] 已自动获取必要第三方依赖；
- [ ] 所有依赖已锁定版本；
- [ ] 所有依赖许可证已记录；
- [ ] 全新环境依赖安装脚本可用；
- [ ] 已建立平台化基础库；
- [ ] Signal Studio 应用可运行；
- [ ] 主要 UI 可在 Qt Designer 预览；
- [ ] UI 已连接真实逻辑；
- [ ] VS Code 可配置、编译和 F5 调试；
- [ ] Debug 构建通过；
- [ ] Release 构建通过；
- [ ] CPU 模式通过；
- [ ] CUDA 环境可用时 GPU 模式通过；
- [ ] 单元测试通过；
- [ ] 集成测试通过；
- [ ] UI 测试通过；
- [ ] 算法验证通过；
- [ ] 性能测试完成；
- [ ] 稳定性测试完成；
- [ ] 安装包可安装并启动；
- [ ] 便携包可启动；
- [ ] 里程碑文档完整；
- [ ] 需求追踪闭环；
- [ ] 代码质量检查通过；
- [ ] CI 可运行；
- [ ] 新工程已推送到默认分支；
- [ ] 正式版本标签已推送；
- [ ] GitHub Release 已创建，或权限阻塞已如实说明；
- [ ] 最终执行报告完整；
- [ ] 没有将失败项伪装为成功。

---

## 19. 最终执行报告

生成：

```text
docs/release/最终执行报告.md
```

至少包括：

1. 总体结论；
2. 开发环境；
3. 输入文档；
4. 测试数据；
5. 自动安装的工具和依赖；
6. 依赖版本和许可证；
7. 关键架构；
8. 基础库；
9. 应用模块；
10. UI 页面；
11. 已实现需求；
12. 未实现需求；
13. 代码统计；
14. 编译结果；
15. 测试结果；
16. 性能结果；
17. 稳定性结果；
18. 安装验证；
19. Git 分支；
20. Git 提交；
21. 原仓库备份分支和标签；
22. 默认分支最新提交；
23. 发布标签；
24. GitHub Release；
25. 发布资产和 SHA256；
26. 已知问题；
27. 环境或权限阻塞；
28. 后续其他工具适配建议。

---

## 20. 立即执行

请立即开始执行，不要复述本文件。

执行顺序：

1. 扫描当前目录；
2. 读取 `../Signal_Studio_开发文档/Signal-Studio-Dev-Docs`；
3. 读取 `../test_data`；
4. 检查本机工具链；
5. 自动下载安装缺失的必要第三方工具和依赖；
6. 接入 `https://github.com/yulongpo/signal_studio.git`；
7. 备份原远程默认分支；
8. 建立开发分支；
9. 初始化平台化工程；
10. 连续完成全部里程碑；
11. 编译、运行、测试并修复；
12. 生成 UI 预览；
13. 制作安装包和便携包；
14. 更新需求追踪；
15. 推送默认分支；
16. 创建标签和 Release；
17. 生成最终执行报告。

不要只输出分析、计划、目录或局部结果。在全部可执行工作完成之前持续推进。
