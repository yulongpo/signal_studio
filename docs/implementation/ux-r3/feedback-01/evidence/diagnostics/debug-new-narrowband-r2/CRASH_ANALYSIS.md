# 新增测试的临时 JSON 引用崩溃

原命令：`scripts/test-ux-r3-directed.ps1 -Configuration Debug -Suite Narrowband -Cases compactAxesCoverEveryPageAndPreserveAnalysis,compactAxesFontFrequencyAndTimeLabelsReuseCaches`。

本次进程退出码为 `-1073741819`（`0xc0000005`）。Qt 文本日志停留于 `initTestCase`，JUnit 文件为空；退出码和这两份原始文件均保留。标准错误的完整调试器输出在当次工具执行输出中，未另存为本目录的完整转录。下列为其中与定位直接相关的摘录：

```text
While testing compactAxesCoverEveryPageAndPreserveAnalysis
Function time: 3491ms Total time: 3496ms
Exception code: 0xc0000005
# 15: QTest::qCompare<QJsonValueRef,QJsonValueRef>()
# 16: NarrowbandAnalysisUiTests::compactAxesCoverEveryPageAndPreserveAnalysis()
```

根因是新增测试使用 `const auto generation=workspace->renderStatistics()["analysisGeneration"]`，持有临时 `QJsonObject` 的 `QJsonValueRef`。改为 `.value("analysisGeneration")` 后保存独立 `QJsonValue`，不改变生产代码或放宽断言。修正后两项定向检查 4/4 通过，随后三项新增检查 5/5、完整 Debug 窄带 UI 17/17 通过；对应原始日志分别保留于 `debug-new-narrowband-final/`、`debug-review-fix/` 和最终完整 CTest 输出。
