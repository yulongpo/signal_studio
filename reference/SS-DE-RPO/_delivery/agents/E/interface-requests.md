# 接口请求

Agent E。没有要求其它 owner 改代码。本轮不改 `detection_task.cpp`、UI，也不改 `beginDetection` 的丢弃语义。

## 向 D

无。`preserveDurableModelEvidence`、`recoverDetectionPublication` 和 `finish` 的 Completed 条件保持现状。窗口完成不是 Completed，这一点由 E 记录，不由 E 改 task。

## 向 C

无。正式结果显示、`suppressFormalDisplay` / `restoreFormalDisplay` 和参数 stale 保持现状。UX-R6 说明见 `ux-r6-contract.md`。

## 向主代理 / G

无必须接口。若 G 为 publication 拒绝加断言，请检查整句以原前缀开头，并包含 `recoveredAnnotations`、`publicationAnnotations`、`recoveredSourceSetId`、`publicationSourceSetId`。数量相同的对象不一致还要包含 `firstDifferingAnnotationIndex` 与 `annotationId`。不要把前缀改成精确全串相等，否则新后缀会误失败。

频率 bin 索引未实现。原因和后续约束在 `tracking-limit-inventory.md`。不要把默认比较上限从 `4000000` 调大当作修复。实现索引时必须同一变更更新 `tests/detection_postprocess_tests.cpp` 里对“不能合并的对仍然计数”的断言，并保持两个现有错误码为响亮失败。

E 没有新增测试文件，也没有改 `CMakeLists.txt`。本任务禁止完整 CMake configure。
