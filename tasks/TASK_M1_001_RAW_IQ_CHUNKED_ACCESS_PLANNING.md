# TASK-M1-001：M1 开发启动核查与 RAW IQ 分块读取任务规划

> **任务编号**：TASK-M1-001  
> **任务类型**：Codex 规划与工程核查任务  
> **项目**：Signal Studio  
> **当前里程碑**：M1——宽带浏览基础  
> **任务状态**：待执行  
> **关联需求基线**：`docs/baseline/BL1.0/`  
> **建议执行分支**：当前开发分支；如需新建分支，使用 `task/m1-raw-iq-planning`  
> **允许修改范围**：文档、构建与测试基础配置  
> **原则上禁止修改**：业务源码、UI 功能源码、DSP 功能源码、正式需求基线  

---

## 1. 任务背景

Signal Studio 已完成以下工作：

- 软件需求基线 BL1.0 已签署并正式生效；
- 正式需求基线已移动至 `docs/baseline/BL1.0/`；
- 文档体系已简化为适合独立开发者维护的结构；
- 根目录已建立 `AGENTS.md`；
- 历史文档目录 `docs/archive/` 已由开发者主动彻底删除；
- 当前准备进入 M1 实际开发阶段。

本任务用于完成开发启动前的仓库核查，并为首个核心模块“RAW IQ 分块读取”建立最小设计、测试约定和可执行任务拆分。

本任务以核查和规划为主，**不得直接实现完整 RAW IQ 读取模块**。

---

## 2. 当前文档结构

当前项目文档结构应为：

```text
docs/
├─ README.md
├─ baseline/
│  └─ BL1.0/
├─ DEVELOPMENT_PLAN.md
├─ ARCHITECTURE.md
├─ UI_DESIGN.md
├─ TEST_PLAN.md
├─ DECISIONS.md
├─ CHANGELOG.md
└─ assets/
```

根目录至少包括：

```text
README.md
AGENTS.md
```

约束：

1. 不得恢复 `docs/archive/`。
2. 不得重新创建历史复杂文档结构。
3. 不得修改正式需求基线正文。
4. 不得引入面向大型团队的审批、评审或治理流程。
5. 只维护当前必要的核心文档。
6. 未验证内容不得描述为已完成或已确定。

---

## 3. 本轮目标

完成以下工作：

1. 检查当前仓库的真实源码、测试和构建结构。
2. 确认项目是否具备可运行的 CMake/Qt 构建入口。
3. 尝试完成一次实际配置、编译和测试。
4. 根据实际仓库结构更新 `docs/ARCHITECTURE.md`。
5. 为 M1 的 RAW IQ 分块读取建立最小可实施设计。
6. 定义 RAW IQ 测试数据生成与保存约定。
7. 将 RAW IQ 分块读取拆分为若干可独立验证的 Codex 开发任务。
8. 明确下一轮应直接实施的唯一主任务。
9. 更新 `DEVELOPMENT_PLAN.md`、`TEST_PLAN.md`、`DECISIONS.md` 和 `CHANGELOG.md`。
10. 提交并推送本轮变更。

---

## 4. 开始前检查

首先执行：

```bash
pwd
git rev-parse --show-toplevel
git status --short
git branch --show-current
git remote -v
git log -5 --oneline
git tag --list
git submodule status
```

随后检查仓库文件：

```bash
find . -maxdepth 3 -type f
```

在 Windows PowerShell 环境下，可使用等效命令：

```powershell
Get-Location
git rev-parse --show-toplevel
git status --short
git branch --show-current
git remote -v
git log -5 --oneline
git tag --list
git submodule status
Get-ChildItem -Recurse -File -Depth 3
```

重点检查：

- 根目录 `CMakeLists.txt`；
- `CMakePresets.json`；
- `src/`；
- `tests/`；
- `tools/`；
- `cmake/`；
- `third_party/`；
- `.github/workflows/`；
- `README.md`；
- `AGENTS.md`；
- `docs/DEVELOPMENT_PLAN.md`；
- `docs/ARCHITECTURE.md`；
- `docs/TEST_PLAN.md`；
- `docs/DECISIONS.md`。

### 4.1 历史文档删除处理

如果 `docs/archive/` 的删除尚未提交，将其视为开发者明确要求的删除。

如果该删除是当前工作区唯一或主要未提交变化，先创建独立提交：

```text
docs: remove obsolete archived documentation
```

不得恢复 `docs/archive/` 中的任何文件。

### 4.2 工作区保护

如果工作区存在与本任务无关的修改：

- 不得覆盖；
- 不得删除；
- 不得加入本轮提交；
- 最终报告中列出。

如果仓库处于未解决的 merge、rebase 或 cherry-pick 冲突状态，立即停止并报告。

---

## 5. 检查实际源码与构建入口

根据仓库真实内容确认并记录：

1. 是否已有主程序入口；
2. 是否已有 Qt Application 或 MainWindow；
3. 当前源码目录如何划分；
4. 是否已有 `core`、`data`、`dsp`、`visualization` 等模块；
5. 是否已有测试框架；
6. 当前测试框架属于 Qt Test、GoogleTest、Catch2、其他或尚未建立；
7. 是否已配置 Qt 6.11；
8. 是否明确使用 MSVC 2022 x64；
9. 是否已有 CMake Preset；
10. 是否存在第三方依赖；
11. 是否存在构建脚本或 CI；
12. 当前代码能否完成最小编译。

所有结论必须来自实际文件或命令结果，不得根据需求文档推测源码现状。

---

## 6. 执行实际构建验证

优先读取现有 README 和 CMake 配置，并使用项目已有构建方式。

如果存在 `CMakePresets.json`，优先使用 Preset。

目标环境：

- Windows 10/11 x64；
- Visual Studio 2022；
- MSVC x64；
- Qt 6.11；
- CMake。

至少尝试：

1. CMake 配置；
2. Debug 或 RelWithDebInfo 编译；
3. 已存在测试；
4. 最小程序启动条件检查。

不得为了构建通过而进行大规模源码修改。

允许的最小修正仅限：

- 明显路径错误；
- 简单 CMake 语法错误；
- 可明确补齐的基础配置；
- 不改变软件功能的小型构建修复。

任何构建修复必须范围小、原因明确、单独记录，并且不得掩盖缺失依赖。

如果当前环境缺少 Qt、MSVC 或其他依赖：

- 记录真实缺失项；
- 给出准确的后续构建命令；
- 不得声称编译成功。

将实际构建和测试结果写入：

- `docs/DEVELOPMENT_PLAN.md`
- `docs/TEST_PLAN.md`

---

## 7. 更新实际模块结构

根据真实仓库更新 `docs/ARCHITECTURE.md` 中的源码模块章节。

必须区分：

### 7.1 已存在模块

仅记录实际目录和源码。

### 7.2 M1 计划新增模块

只列当前确实需要的模块。可参考：

```text
src/
├─ app/
├─ core/
│  └─ data/
└─ visualization/

tests/
├─ unit/
└─ test_data/
```

若当前项目已有不同且合理的结构，应沿用现有结构。

不得为匹配文档而无意义重构源码。

### 7.3 后续模块

只做简要说明，不提前设计大量接口或创建大量空目录。

---

## 8. RAW IQ 分块读取最小设计

在 `docs/ARCHITECTURE.md` 中完善“RAW IQ 文件访问”章节。

本轮设计范围限定为 M1 最小可用能力。

### 8.1 支持的数据布局

首批规划：

- 复数交织 IQ；
- 复数交织 QI；
- `I0, Q0, I1, Q1, ...`；
- `Q0, I0, Q1, I1, ...`。

本轮暂不覆盖多通道非交织复杂格式。

### 8.2 首批数据类型

M1 首批规划：

- Int8；
- Int16；
- Float32。

需要明确：

- 每个分量字节数；
- 每个复样本字节数；
- 有符号性；
- 是否归一化；
- 转换为内部类型的方法。

内部计算类型优先评估：

```cpp
std::complex<float>
```

如果证据不足，应标记为“待验证”。

### 8.3 字节序

规划支持：

- Little Endian；
- Big Endian。

Windows/x86 默认使用 Little Endian。

### 8.4 文件参数模型

至少包括：

- 文件路径；
- 数据类型；
- IQ/QI 顺序；
- 字节序；
- 文件头偏移；
- 采样率；
- 中心频率；
- 起始时间；
- 幅度缩放；
- 是否复数；
- 文件大小；
- 有效样本数。

### 8.5 样本索引规则

必须使用 64 位整数。

至少明确：

- 原始复样本索引；
- 字节偏移；
- 请求起始样本；
- 请求样本数量；
- 实际返回样本数量；
- 文件尾处理；
- 越界处理。

字节偏移：

$$
O_{\mathrm{byte}}
=
O_{\mathrm{header}}
+
n_{\mathrm{start}} B_{\mathrm{sample}}
$$

其中：

- $O_{\mathrm{byte}}$：文件读取字节偏移；
- $O_{\mathrm{header}}$：文件头偏移；
- $n_{\mathrm{start}}$：起始复样本索引；
- $B_{\mathrm{sample}}$：每个复样本字节数。

### 8.6 最小分块读取接口

设计一个明确、可测试的同步核心接口。可参考：

```cpp
struct RawIqFormat {
    SampleDataType dataType;
    IqOrder iqOrder;
    ByteOrder byteOrder;
    std::uint64_t headerOffsetBytes;
    double sampleRateHz;
    double centerFrequencyHz;
    double amplitudeScale;
};

struct SampleRange {
    std::uint64_t startSample;
    std::uint64_t sampleCount;
};

struct ReadResult {
    std::vector<std::complex<float>> samples;
    std::uint64_t actualStartSample;
    std::uint64_t actualSampleCount;
    bool reachedEndOfFile;
};

class RawIqReader {
public:
    explicit RawIqReader(...);

    FileInfo inspect() const;
    ReadResult read(const SampleRange& range) const;
};
```

结合项目实际风格评估：

- 返回值模型；
- 错误处理；
- 是否使用 `std::expected` 或兼容实现；
- 是否使用异常；
- 是否使用错误码；
- 是否允许调用方复用缓冲区；
- 是否支持取消。

本轮不设计完整异步读取 API。

### 8.7 文件合法性检查

至少包括：

- 文件存在；
- 文件可读；
- 文件头偏移不超过文件大小；
- 有效数据长度能否整除复样本字节数；
- 文件尾不完整样本；
- 采样率是否合法；
- 起始索引越界；
- 请求范围溢出；
- 64 位乘法溢出。

### 8.8 内存约束

明确：

- 不允许完整加载大文件；
- 单次读取大小受上限控制；
- 默认分块大小由基准测试确定；
- 读取内存与请求样本数线性相关；
- 内存不得随文件总大小无界增长。

### 8.9 线程与并发

M1 首批允许使用同步核心读取接口，但必须：

- 不在 UI 主线程直接执行大块读取；
- Reader 不直接操作 QWidget；
- 明确并发读取实例的安全边界；
- 不提前设计复杂线程池。

### 8.10 错误模型

至少规划：

- `FileNotFound`；
- `PermissionDenied`；
- `InvalidFormat`；
- `InvalidHeaderOffset`；
- `IncompleteSample`；
- `RangeOutOfBounds`；
- `ReadFailure`；
- `ArithmeticOverflow`。

错误处理形式应登记为当前决策或待验证决策。

---

## 9. RAW IQ 测试数据约定

在 `docs/TEST_PLAN.md` 中新增“RAW IQ 测试数据约定”。

测试数据原则：

- 不提交超大二进制文件；
- 通过脚本确定性生成；
- 固定随机种子；
- 小型固定样本可以进入仓库；
- 大型性能样本只生成，不提交。

建议结构：

```text
tests/test_data/
├─ README.md
├─ manifests/
└─ generated/
```

其中：

- `generated/` 加入 `.gitignore`；
- `manifests/` 保存测试数据参数和校验信息。

至少定义以下测试样本：

1. Int8 Little Endian IQ；
2. Int8 QI；
3. Int16 Little Endian IQ；
4. Int16 Big Endian IQ；
5. Float32 Little Endian IQ；
6. 带文件头偏移；
7. 空文件；
8. 只有文件头；
9. 文件尾缺少一个分量；
10. 文件尾存在多余字节；
11. 请求最后一个样本；
12. 请求越过文件尾；
13. 超大样本索引；
14. 可验证的递增 I/Q 序列；
15. 正弦复信号，用于后续 FFT 验证。

每个测试数据定义：

- 文件名；
- 数据类型；
- IQ/QI；
- 字节序；
- 头偏移；
- 样本数量；
- 采样率；
- 中心频率；
- 预期前若干样本；
- SHA-256；
- 生成参数。

优先使用 Python 标准库和 NumPy 生成。

本轮仅定义方案。只有在仓库已经具备明确 Python 工具体系时，才允许创建最小生成脚本。

---

## 10. 单元测试规划

在 `docs/TEST_PLAN.md` 中规划：

- `TEST-DATA-001`：文件信息解析；
- `TEST-DATA-002`：Int8 IQ 读取；
- `TEST-DATA-003`：Int16 Little Endian 读取；
- `TEST-DATA-004`：Int16 Big Endian 读取；
- `TEST-DATA-005`：Float32 读取；
- `TEST-DATA-006`：IQ/QI 顺序；
- `TEST-DATA-007`：指定样本区间读取；
- `TEST-DATA-008`：文件尾边界；
- `TEST-DATA-009`：不完整样本；
- `TEST-DATA-010`：非法参数；
- `TEST-DATA-011`：超大索引和整数溢出；
- `TEST-DATA-012`：内存受控验证；
- `TEST-DATA-013`：重复随机访问一致性。

每项测试至少写明：

- 关联需求；
- 输入；
- 操作；
- 预期结果；
- 当前状态。

当前状态只能填写“待实现”或“待执行”。

---

## 11. 技术决策登记

在 `docs/DECISIONS.md` 中检查或新增：

### DEC-DATA-001 RAW IQ 内部样本类型

候选：

- `std::complex<float>`；
- 分离的 float I/Q；
- 保持原始类型；
- 模板化样本容器。

### DEC-DATA-002 文件访问方式

候选：

- `std::ifstream` 分块读取；
- Windows 文件映射；
- Qt `QFile`；
- 分块读取与内存映射混合。

M1 可优先选择最简单、可测试的同步分块读取方案，但不得把未经验证的方案写成最终高性能架构。

### DEC-DATA-003 错误处理模型

候选：

- 异常；
- 错误码；
- `std::expected` 或兼容实现；
- 自定义 Result。

### DEC-DATA-004 缓冲区策略

候选：

- 每次返回 vector；
- 调用方提供缓冲区；
- Reader 复用内部缓冲区。

### DEC-DATA-005 测试数据生成方式

建议：

- 确定性脚本生成；
- 小型数据可提交；
- 大型数据不提交。

证据不足时，状态必须保持“待验证”。

---

## 12. 实施任务拆分

在 `docs/DEVELOPMENT_PLAN.md` 中建立以下任务。

### M1-DATA-001 构建与测试基础确认

目标：

- 确认数据模块目录；
- 确认测试框架；
- 确认测试目标可由 CTest 执行；
- 补齐最小构建配置。

完成条件：

- 测试目标能够构建；
- 至少一个基础测试可执行；
- 不涉及 RAW IQ 功能实现。

### M1-DATA-002 RAW IQ 测试数据生成器

目标：

- 创建确定性测试数据生成工具；
- 创建数据清单；
- 生成 Int8、Int16、Float32 基础样本；
- 支持 IQ/QI、字节序和头偏移。

完成条件：

- 测试数据可重复生成；
- SHA-256 稳定；
- 不提交大型生成文件。

### M1-DATA-003 RAW IQ 格式模型与文件检查

目标：

- 实现格式枚举和参数模型；
- 实现文件大小、样本数和格式合法性检查；
- 实现溢出检查。

完成条件：

- 合法和非法文件均有单元测试。

### M1-DATA-004 同步分块读取核心

目标：

- 按样本索引读取指定范围；
- 支持 Int8、Int16、Float32；
- 支持 Little/Big Endian；
- 支持 IQ/QI；
- 转换为统一内部样本类型。

完成条件：

- TEST-DATA-002 至 TEST-DATA-009 通过。

### M1-DATA-005 边界、错误和内存验证

目标：

- 文件尾；
- 空文件；
- 不完整样本；
- 超大索引；
- 越界请求；
- 读取上限；
- 重复随机访问。

完成条件：

- 边界测试通过；
- 内存不随文件总大小增长。

### M1-DATA-006 应用层接入准备

目标：

- 为后续 RAW 导入对话框和 DataSource 建立稳定调用入口；
- 不实现完整 UI；
- 不在 UI 主线程执行大块读取。

完成条件：

- 核心读取模块可独立使用；
- 接口不依赖 QWidget。

每项任务必须记录：

- 关联需求；
- 输入；
- 输出；
- 预计修改文件；
- 测试；
- 完成条件；
- 前置依赖。

---

## 13. 确定下一轮唯一主任务

默认下一轮任务：

```text
M1-DATA-001：构建与测试基础确认
```

如果仓库已经具备稳定测试框架和 CTest 集成，则下一轮改为：

```text
M1-DATA-002：RAW IQ 测试数据生成器
```

不得跳过测试基础，一次性实现全部读取模块。

将最终选择写入 `docs/DEVELOPMENT_PLAN.md` 的“Codex 下一轮任务”。

---

## 14. 允许修改范围

本轮允许修改：

- `docs/DEVELOPMENT_PLAN.md`
- `docs/ARCHITECTURE.md`
- `docs/TEST_PLAN.md`
- `docs/DECISIONS.md`
- `docs/CHANGELOG.md`
- `docs/README.md`
- `README.md`
- `AGENTS.md`

仅在执行最小构建修复或测试基础确认时，允许修改：

- `CMakeLists.txt`
- `CMakePresets.json`
- `cmake/`
- `tests/`
- `.github/workflows/`

本轮原则上不得修改：

- `docs/baseline/BL1.0/`
- 业务源码；
- UI 功能源码；
- DSP 功能源码；
- 与 RAW IQ 规划无关的代码。

如果修改构建文件，必须说明原因和验证结果。

---

## 15. CHANGELOG 更新

在 `docs/CHANGELOG.md` 的 `Unreleased / Documentation` 中记录：

- 完成 M1 开发启动核查；
- 根据真实仓库更新模块结构；
- 完成 RAW IQ 分块读取最小设计；
- 完成测试数据约定；
- 完成实施任务拆分。

不得把规划描述为功能实现。

---

## 16. 验证要求

完成后执行：

1. 检查 Git 状态；
2. 确认 `docs/archive/` 未被重新创建；
3. 确认正式基线没有修改；
4. 检查所有 Markdown 相对链接；
5. 检查文档中的源码目录与实际仓库一致；
6. 检查未执行测试没有标记为通过；
7. 检查未确定方案仍标记为待验证；
8. 检查任务依赖顺序合理；
9. 检查下一轮只有一个明确主任务；
10. 检查未创建大量无意义空目录或文件；
11. 输出实际构建命令和结果；
12. 输出实际测试命令和结果；
13. 确认未修改无关源码。

---

## 17. Git 提交

如果历史归档删除尚未提交，先提交：

```text
docs: remove obsolete archived documentation
```

完成本轮任务后提交：

```text
docs: plan M1 RAW IQ chunked data access
```

要求：

- 只添加本轮相关文件；
- 不修改 `requirements-bl1.0` 标签；
- 不创建新的需求基线标签；
- 不强制推送；
- `origin` 可用时推送当前分支；
- 推送失败时保留本地提交并报告。

---

## 18. 最终执行报告

最终报告必须包括：

1. 执行结果；
2. 当前实际源码目录结构；
3. 当前构建入口；
4. 当前测试框架；
5. 实际构建命令；
6. 实际构建结果；
7. 实际测试命令；
8. 实际测试结果；
9. RAW IQ 最小设计摘要；
10. 测试数据约定摘要；
11. M1-DATA-001 至 M1-DATA-006 任务清单；
12. 下一轮唯一主任务；
13. 修改文件；
14. 正式基线未修改确认；
15. `docs/archive/` 未恢复确认；
16. Git 提交哈希；
17. GitHub 推送状态；
18. 仍需人工确认的问题。

---

## 19. 阻断条件

除非出现以下情况，否则不要中途询问：

- 当前不是 Git 仓库；
- 仓库存在未解决冲突；
- 正式基线被意外修改；
- 即将覆盖已有且内容不明的文件；
- 实际仓库结构与现有文档严重冲突，无法可靠判断；
- 构建修复需要大规模修改业务源码。

出现阻断条件时，应立即停止修改并输出：

- 阻断原因；
- 已完成步骤；
- 当前 Git 状态；
- 建议的人工处理方法。
