# UX-R6 / R6-0 基线与实际复现

2026-10-26，Windows / Asia/Shanghai。已完整读取 `reference/ux-r6/` 四份原始文件及当前 AGENTS、UX-R5 设计/生命周期和 UX-R4 交付合同。四份 reference 保持原文，纳入本轮版本记录；任务包 ZIP 和其它用户资料不改动。

本地分支 `rebuild/a143-native`；开始及 fetch 后本地/远端均为 `ec511662d4d84d5b9757576d24ed2124b6d8a029`，左右差异0/0，跟踪文件干净。原有四项用户资料及新增 UX-R6 ZIP/参考目录均已登记。禁止 reset、clean、force push；本轮最终独立审核前禁止远端写入。

工具链延用 Visual Studio 18 2026、Qt 6.11.1 EXACT 和公共 presets。实际推理设备为 NVIDIA GeForce RTX 5060 Laptop GPU（8151 MiB），驱动591.84，现有 TensorRT10.11.0.33 / CUDA runtime12090。显示GPU另行原生验证，不能混同推理GPU。

## 用户原数据和历史日志

用户明确提供 `E:/sig_studio_test2/new_test/test.ssproj`。只读读取工程及其权威资产目录，不覆盖或重新保存该工程。真实源文件存在：

`D:/coding/FFSCN_train2/FFSCN_TEST_GUI/test_data/data/20260728-045101-650_bw_20000000_sampleTime_0.8_rollOff_0.35.wav`

原数据为20 MS/s、16,000,000复数样本、交替小端int16 IQ、44字节WAV头、中心频率0，共64,000,044字节。数据来自用户模型测试目录；没有把“用户原始文件”解释成现场RF采集证据。

用户工程资产包含13个运行，两个65536 / 0.25ms / batch2 / threshold0.5失败均为164/639窗口、`inference`阶段、`Detection mappings exceed evidence budget`。原日志的时间策略为 **ROI**，与“时间精修”症状描述区分；后续必须另测Sample，不能把ROI记录冒称Sample复现。其他8192/0.25ms曾完整639/639，但不是本轮新执行。

## 本轮未经产品修改的重跑

新增 `SignalStudioDetectionBenchmark` 仅为独立headless入口，链接当前产品Infrastructure，读取原运行冻结请求，将新run写入空的任务目录；只改变runId并重新绑定同一部署模型，未修改模型/产品算法/原项目。Release编译通过。命令和原始结果见 `evidence/baseline/`：

```powershell
./out/vs2026-qt611-release_bin/SignalStudioDetectionBenchmark.exe --request out/tasks/ux-r6/diagnostics/run-0c82d126-38e7-4b14-8778-491fc5c670f3-original-manifest.json --output out/tasks/ux-r6/diagnostics/r6-0-baseline-user-roi --timeout 900
```

实际131293ms，Failed，164/639，原样复现相同存储预算错误。持久化前缀可投影21条**时间未定位**观察、0活动段；这些不是成功正式结果，CLI退出1，未更改用户当前正式集合。主进程峰值RSS53,985,280字节，不包括独立worker。GPU实测窗口内聚合已用显存3035/8151MiB，利用率峰80%；采样在启动后约10.274秒才开始，没有覆盖冷启动，也不是单worker显存归因。

BUG02由并行诊断Agent Goodall实际运行生产Qt表模型/服务探针：非空旧正式集合1条，在参数提交和模型切换后仍存储1条但可见行0，保存重开仍隐藏；未提交/撤销草稿保持1行。旧有测试还明确断言隐藏，不能将其绿灯当作R6通过。诊断仅用临时夹具，不是模型/GPU/实屏证据。

R6-0完成的是基线与故障确认；尚未修复、尚未性能优化、尚未独立终审、尚未推送。详见 ROOT_CAUSE 和 EXECUTION_STATUS。
