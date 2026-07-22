# 自动化校验报告

| 元数据项 | 内容 |
|---|---|
| 文档编号 | SS-VAL-001 |
| 文档名称 | 自动化校验报告 |
| 项目名称 | Signal Studio / Signal Platform |
| 文档版本 | V1.0.0 |
| 基线版本 | BL1.0 |
| 状态 | 已批准 |
| 内容类型（meta.contentType） | Reference |
| 编制日期 | 2026-07-22 |
| 适用阶段 | 交付验收 |
| 输入来源 | 本目录自动化扫描与结构化基线 |
| 本版变更 | 自动生成、审核并批准文档基线 |

## 1. 结论

自动校验项 34：PASS 34，WARN 0，FAIL 0。校验范围覆盖文件、Markdown 元数据与链接、编号/追踪、Mermaid 结构、架构 DAG、API 第三方泄漏、Excel、SVG/PNG/HTML、许可证、依赖锁、测试数据和原始副本。

## 2. 明细

| 类别 | 检查 | 结果 | 详情 |
|---|---|---|---|
| 文件 | 必需交付物 | PASS | 44/44 存在；缺失=[] |
| 文件 | 空目录 | PASS | 空目录=[] |
| 文件 | Markdown 实质内容 | PASS | 低于 800 字节=[] |
| 编号 | 需求编号唯一 | PASS | 重复=[]; 总数=198 |
| 编号 | API/测试编号唯一 | PASS | API重复=[]; 测试重复=[] |
| 追踪 | 无悬空引用 | PASS | 悬空=[] |
| 追踪 | 覆盖率 | PASS | {"requirements": 198, "p0": 158, "p1": 39, "page_coverage": 1.0, "api_coverage": 1.0, "test_coverage": 1.0, "library_coverage": 1.0, "reuse_coverage": 1.0, "p0_p1_test_coverage": 1.0} |
| 文档 | 元数据 | PASS | 缺失=[] |
| 批准 | 全部 Markdown 已批准 | PASS | 未批准=[]; 总数=57 |
| 批准 | 全部 Markdown 使用 BL1.0 | PASS | 错误=[] |
| 写作 | 内容类型有效 | PASS | 异常=[] |
| 写作 | 可适用 Writing Guidelines 规则 | PASS | 异常=[] |
| 文档 | 参考/未决/变更尾部 | PASS | 缺失=[] |
| 链接 | 相对链接 | PASS | 无效=[] |
| 文档 | 无绝对路径 | PASS | 违规=[] |
| 文档 | 无占位符 | PASS | 违规=[] |
| Mermaid | 结构语法 | PASS | 异常=[] |
| 架构 | 依赖节点完整 | PASS | 缺失=[] |
| 架构 | 无循环依赖 | PASS | 循环=[] |
| 架构 | 公共库无应用品牌 | PASS | 违规=[] |
| API | 公共签名无第三方类型 | PASS | 泄漏=[] |
| 架构 | 十模块齐全 | PASS | 实际=['SignalCompute', 'SignalCore', 'SignalDSP', 'SignalData', 'SignalDataset', 'SignalModelRuntime', 'SignalPluginSDK', 'SignalTaskRuntime', 'SignalVisualization', 'SignalWorkbench'] |
| 编号 | ADR-001 至 ADR-020 | PASS | 实际=['001', '002', '003', '004', '005', '006', '007', '008', '009', '010', '011', '012', '013', '014', '015', '016', '017', '018', '019', '020'] |
| Excel | 可打开/样式/公式/下拉/零错误 | PASS | 问题=[] |
| SVG | 可解析/视口/许可证 | PASS | 问题=[] |
| PNG | 可打开 | PASS | 数量=25; 问题=[] |
| 截图 | 真实浏览器标准截图 | PASS | 数量=5; 尺寸=[(1280, 720), (1600, 900)] |
| HTML | 归档可运行结构 | PASS | 字节=391687 |
| 许可证 | 依赖许可证/来源/锁定 | PASS | 依赖=20; 默认选用=14; 缺项=[] |
| 许可证 | 原创资产许可证 | PASS | license=LicenseRef-Signal-Studio-Project |
| 测试数据 | 生成数据哈希 | PASS | 文件=3; 问题=[] |
| 原始材料 | 只读副本哈希 | PASS | 副本=12; 变化=[] |
| 文档 | validation-report.md | PASS | 报告元数据和尾部结构 |
| 文档 | 执行报告.md | PASS | 报告元数据和尾部结构 |

## 3. 追踪指标

```json
{
  "requirements": 198,
  "p0": 158,
  "p1": 39,
  "page_coverage": 1.0,
  "api_coverage": 1.0,
  "test_coverage": 1.0,
  "library_coverage": 1.0,
  "reuse_coverage": 1.0,
  "p0_p1_test_coverage": 1.0
}
```

## 4. 校验边界

Mermaid 做结构语法检查；Excel 由 openpyxl 打开并扫描公式错误。本机 LibreOffice 辅助脚本因 Windows `AF_UNIX` 不可用，已改用已安装 Excel COM 执行 `CalculateFullRebuild` 并保存。HTML 浏览器标准截图由 Playwright/Edge 产生。生产 C++ 构建、真实 FFT/STFT、GPU、模型、100 GB 数据和性能未在文档仓库中执行。

## 参考资料

- 本目录 `validation-results.json`
- 本目录结构化 requirements/api/architecture/test/asset/dependency 清单

## 未决事项

- 生产软件尚未实现；本报告只证明文档交付资产校验结果。

## 变更记录

| 版本 | 日期 | 变更 |
|---|---|---|
| V1.0.0 | 2026-07-22 | 建立并自动审核通过平台化开发基线，纳入需求、接口、测试和复用边界。 |
