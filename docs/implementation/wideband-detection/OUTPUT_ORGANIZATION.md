# M2后输出目录整理

在M2自检和本地提交`cc103e3`完成后执行，在M3恢复前完成。本次任务来自用户2026-10-13新增要求；后续目录规则见[output-layout.md](../../architecture/output-layout.md)，项目AGENTS.md同步约束。

165项旧根日志/目录已迁移。正式Debug/Release构建树及`_bin/`保留原路径。历史验证按feature归档，M1/M2日志和局部探针归对应节点；M3预备模型/runtime与本地探针、M3/M4隔离草稿分别归对应节点。这些预备文件不记为M3/M4执行或验收完成。

| 整理检查 | 结果 |
|---|---|
| 绝对路径及覆盖检查 | 所有源/目标限于工作区out；拒绝reparse路径/覆盖；迁移计划不含公共构建路径 |
| 迁移与重新复核 | 165/165记录存在，目录文件数/总字节及单文件SHA-256一致 |
| 模型资产保护 | ONNX、FP32、FP16候选的三份SHA-256与原冻结记录一致；只证明字节保存，不证明推理通过 |
| 输出根目录 | 仅4个正式构建/部署目录、tasks、toolchains、archive及README索引 |
| 构建入口 | `scripts/build.ps1 -Configuration Debug -Action Build`退出0 |
| Python | 用户指定`D:/profiles/anaconda/python.exe`优先；M0 schema依赖由`out/toolchains/wideband-python/Lib/site-packages`显式提供，契约校验通过；迁移后原工具环境亦通过 |
| 脚本/文本 | organize-output.ps1 PowerShell AST无错误，Git空白检查通过 |

本次无产品代码改动，因此使用目录身份、工具环境和既有构建入口检查；M2已经完成的15/15 CTest、94/94 UI、23场景证据留在M2。每次节点运行的中间产物直接输出到节点目录，最终需要审查的报告才归档docs。

完整迁移记录：[output-relocation.json](output-relocation.json)。本地计划、原隔离说明和验证日志保留在`out/tasks/output-organization/20261013/`；文件原名和历史证据中的原路径可通过迁移记录查找。
