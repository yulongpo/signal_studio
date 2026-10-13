# 构建输出与任务中间产物

2026-10-13起执行。用户要求M2完成后先整理`out/`，后续节点直接按此规则产出。入口索引位于被忽略的`out/README.md`；需要随Git审查的最终证据保存到对应`docs/implementation/`节点。

| 目录 | 内容 |
|---|---|
| `out/vs2026-qt611-debug/`、`out/vs2026-qt611-release/` | 公共CMake presets的构建树、CTest内部文件和生成器缓存 |
| `out/vs2026-qt611-debug_bin/`、`out/vs2026-qt611-release_bin/` | 正式可执行文件、Qt/CRT部署结果 |
| `out/portable-ninja/` | 跨平台预设构建树，仍未验收Ubuntu运行 |
| `out/tasks/wideband-detection/<node>/logs/` | 该节点构建、回归、截图、模型转换/探针的控制台日志 |
| `out/tasks/wideband-detection/<node>/diagnostics/` | JSON/JSONL、数值差异、计时、资源与临时golden诊断 |
| `out/tasks/wideband-detection/<node>/assets/` | 该节点模型、runtime、输入张量及转换资产；显式manifest/hash管理 |
| `out/tasks/wideband-detection/<node>/local-builds/` | 独立探针和临时验证工具的二进制/对象，不混入正式工程输出 |
| `out/tasks/wideband-detection/<node>/deferred/` | 隔离草稿，只能在前置节点提交后恢复；目录存在不代表节点执行或通过 |
| `out/toolchains/` | 可复用工具环境，如`wideband-python/`；用`python -m pip`管理该环境 |
| `out/tasks/history/<feature>/` | 本次整理前的显示/谱分析/参数输入等验证记录，保留原文件名 |
| `out/archive/builds/`、`out/archive/temp/` | 历史非公共构建树、部署快照与临时文件 |
| `out/tasks/output-organization/<date>/` | 迁移计划、逐项迁移索引和整理自检日志 |

`<node>`使用M0、M0-P、M1–M5。直接调用现有`scripts/build.ps1`、`screenshot.ps1`、`gpu_smoke.ps1`和`verify-package.ps1`，把控制台输出定向到节点`logs/`。各脚本自己的正式构建/捕获结果继续使用其规定路径；测试程序在CMake构建树内的自动日志属于构建结果。退出节点时将最终报告/必要截图归档到`docs/implementation/wideband-detection/<node>/`，中间失败或旧报告留在本地节点目录。本机Python任务优先使用用户指定的`D:/profiles/anaconda/python.exe`；M0冻结的schema依赖保留在工具目录，必要时显式配置该目录。

示例：

```powershell
$nodeRoot = 'out/tasks/wideband-detection/M3'
New-Item -ItemType Directory -Path "$nodeRoot/logs", "$nodeRoot/diagnostics" -Force
./scripts/build.ps1 -Configuration Debug -Action All *> "$nodeRoot/logs/debug-build-test.txt"
```

不向`out/`根目录新增节点日志、模型、工具或临时工程。历史证据中的原路径保留其当时含义，查找迁移位置使用`out/tasks/output-organization/20261013/relocation.json`；这次迁移不改写冻结记录的字节。历史CMake缓存仍记录原绝对目录，归档用于查阅；新的正式构建使用公共presets重新配置。

`scripts/organize-output.ps1`默认只写计划，`-Apply`执行已知旧目录整理。它核对绝对边界、拒绝reparse路径/覆盖、排除正式构建目录，并逐项记录验证结果；后续正常节点直接使用上述目录，不依赖事后整理。
