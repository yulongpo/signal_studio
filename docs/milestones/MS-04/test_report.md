# MS-04 测试摘要

本地最终矩阵无失败：

| 范围 | 结果 |
|---|---:|
| CPU Debug 全量 | 243/243 |
| CPU Release 全量 | 243/243 |
| CUDA Debug MS-04 + Compute | 50/50 |
| CUDA Release MS-04 + Compute | 50/50 |
| 最终四套 CPU/CUDA MS-04 标签 | 各 38/38 |
| MS-04 批准直接需求 | 20/20 |
| 8 小时稳态 | 70,024 次，标准错误 0 字节 |

安装消费、外部录制、高 DPI、默认 Windows 平台、格式、静态分析、公共头、基线、
依赖锁、本机预设、VS Code、CI 静态契约和差异检查均通过。详细环境、用例和诚实边界
见 [MS-04_测试报告.md](MS-04_测试报告.md)。

精确代码提交 `04932be1f611826359c8689d1fb4116ce23d695f` 已由 GitHub Actions 运行
`30450012411` 验证，`headless-build-test` 与
`windows-ui-module-performance` 均为 success。
