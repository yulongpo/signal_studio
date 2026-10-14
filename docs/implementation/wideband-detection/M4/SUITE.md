# M4 顺序离线验证套件

入口为 `tools/detection/time_suite.py`。套件读取 ready execution freeze，并严格复核自身、C++ CLI、reference、evaluator、epoch-1200 checkpoint、G input-manifest及完整T manifest的SHA。它不建立或放宽 execution freeze，不构建、不提交，不允许从未知旧目录采用结果。

## 调用与矩阵

```powershell
& 'D:/profiles/anaconda/python.exe' -B tools/detection/time_suite.py `
  --freeze 'docs/implementation/wideband-detection/M4/execution-freeze.json' `
  --root 'out/tasks/wideband-detection/M4/diagnostics/suite-debug-v1' `
  --configuration Debug
```

`Debug|Release` 分别选择相应 `_bin/SignalStudioDetectionTimePipelineTests.exe`。Python子任务使用当前 `sys.executable`。默认 `--scope all`；可以固定一个范围，以独立目录单独调度：

| scope | 输入 | strategy | short | 对数 |
|---|---|---|---|---:|
| G-sample | 完整G10/.8 s | sample | false | 10 |
| G-roi | 完整G10/.8 s | roi | false | 10 |
| G-coarse | 完整G10/.8 s | coarse | false | 10 |
| G-no-split | 完整G10/.8 s | no-split | false | 10 |
| G-roi-no-split | 完整G10/.8 s | roi-no-split | false | 10 |
| G-coarse-no-split | 完整G10/.8 s | coarse-no-split | false | 10 |
| T-default | 完整合成T96 | sample | false | 96 |
| T-sample-short | T短专集32 | sample | true | 32 |
| T-roi-short | T短专集32 | roi | true | 32 |

合计220对。一次一文件：先 native CLI，结束并清理其进程树后，再启动独立 reference；不并行GPU。reference固定 `cuda:0`、`product-v2`、2 GiB sample预算；native CLI的request预算必须为128 MiB evidence/256 MiB work。默认sample参数仍是GUI数学基线，批准参数扩展见 `PARAMETER_DECISION.md`。

G使用冻结的全部10 WAV与原TXT，必须20 Msps、16,000,000样本、.8 s、594真值事件。T要求 `signalstudio-m4-independent-holdout-v1` complete=true、96文件，并为每项核验WAV、TXT和私有sample-exact JSON；评价的真值使用该私有JSON，不使用TXT量化值。短专集完整32文件独立选择。T保持合成身份，不填补真实RF采集能力。

`--max-files N` 每个选择的scope只取按sampleId排序的前N项，**始终为诊断模式**，即使N不小于某组总数。scope/suite summary标示 `diagnosticPartial`、`acceptanceEligible:false`、`diagnostic-partial-no-gate`；evaluator内的点值和阈值比较只是所选子集的数值，不能作为完整L3/L4通过结论。正式矩阵使用无此参数的新root。

## 结果与恢复

`--root` 下保存不可变 `suite-plan.json`；每scope保存 `scope-manifest.json`、`paired-index.json`、`evaluation.json`、评价identity/completion/log及 `scope-summary.json`。每文件独立保存 `native/`、`reference/` 的launch、process identity、process.log、completion、resources.jsonl与工具result。`pair-run-records.json` 和 `suite-summary.json` 是持续更新的进度摘要；原launch/completion/result/log不覆盖。

同参数再次调用原root即恢复。已有stage必须匹配原完整source/truth、execution freeze SHA、native/reference代码SHA、configuration、strategy/short/semantics及固定预算；成功result的内容hash和metadata还必须一致。任何不一致要求新root，不自动改用新结果。

已结束失败或结果缺失照原样计入，不自动重试。中断且没有completion的stage保留失败历史；如果其记录PID仍存活，阻止后续GPU任务。未产生result仍在paired index保存原路径，evaluator按零预测保留全部真值。若一个失败/超时子进程恰好留下pass=true的result，原文件保留，另存 `failure-accounting-result.json` 的pass=false计账副本；index同时保留original与计账路径，不能让失败任务获取预测成绩。

评价也是一次性的：固定index/freeze/evaluator身份并保存报告hash；恢复时复用匹配的报告和原log，失败/中断评价保留原因，不静默重算替换历史。每scope独立调用固定评价器，保留所有纳入sampleId、零检测/未知/失败记录与分组、配对bootstrap。数值报告不直接宣布整体验收通过；reference失败不能建立迁移parity。

## 超时与资源

默认 `--timeout-seconds 1500` 应用于每个native/reference子进程；scope评价限300秒。Windows子进程加入独立kill-on-close Job，正常退出、超时或中断后清理其子树，包括worker，再进入下一GPU阶段。套件只处理自己创建的子进程，不杀其它模型任务。窗口隐藏，stdout/stderr原样写入每run日志。

每5秒尝试采样主PID RSS和 `nvidia-smi` 整设备VRAM/利用率；RSS不包含全部worker，整设备显存可能包括其它进程与桌面，因此明确记录measurement scope，不把它们当作该算法独占分配。不可用时为null。无需psutil。`--no-resource-sampling` 可关闭这两项，仍保留wall time、退出码、超时和失败原因。

## 工具检查

```powershell
& 'D:/profiles/anaconda/python.exe' -B tools/detection/time_suite.py --self-check
```

CPU自检已通过：精确9组/220对、partial永不作完整gate、失败恢复保留、配置/freeze变化拒绝、迟到result拒绝、中断历史保留、失败计账副本保留原件，以及仅用可删除Python sleeper验证Windows进程树超时清理。自检没有启动Qt CLI、模型、正式G/T或GPU，也没有调用显存查询。文件经过Black80与py_compile；正式执行由主任务在最终execution freeze之后调度。
