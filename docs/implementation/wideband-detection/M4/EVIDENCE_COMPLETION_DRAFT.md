# M4 证据补项独立草稿

位置：`out/tasks/wideband-detection/M4/deferred/evidence-r4/`；独立构建源与输出：`out/tasks/wideband-detection/M4/local-builds/evidence-r4/source/`。只处理 M4，尚未应用到正在执行 R3 的 canonical 源码或部署目录，未提交节点。

草稿保存现有 ROI 算法已经算出的 positiveMean、fractionOff、时间比较门限及频率比较门限，附 `positive-db-residual-mean-v1` 数值域。掩码比较表达式及三次去均值/截正、平滑、时长过滤、源坐标映射均保持；没有加入新噪底算法或 ROI 曲线。辅助证据显示实际门限，明确它们不是 sample 线性功率噪底；同时将 Coarse 的已有噪底/门限正确标为谱密度 dB 域。

真实 Coarse 回退尝试的测量、状态、失败原因及 dB 数值域也保存，包括无可靠活动的尝试；只有 Applied 才拷贝活动段。该补项帮助核对未知时间的来源，不从失败测量伪造活动，不改原 mask/活动决策。最新相关 CTest 为 2/2、1.21 秒，见 `logs/evidence-r4-final-actual-ctest.log`。

2026-10-14 补验：增加同一实际 CI16 读取/STFT 路径上的全零源，分别执行 Sample/ROI 无可靠结果、独立 Coarse 也无可靠结果的组合。断言保存内层 fallback 状态、`coarse-no-supported-on-run`、实际 noise/threshold、数值域和固定参数政策；活动段和原始活动都为空，保留模型窗口观察，且不能声称 measured-coarse-fallback 活动。相关 CTest 2/2、1.61 秒，见 `logs/evidence-r4-silent-fallback-ctest-r2.log`。首轮新增测试误期望观察质量为 Unknown，违反既有模型窗口 `WindowObservation` 契约；已修正测试，首轮失败日志保留，未修改产品算法。

通过复制的 `scripts/build.ps1 -Configuration Release -Action Build -Jobs 2` 完成全部目标重新链接，主程序、UiTests、UiCapture 的 UTC 更新时间为 23:25:42–45，晚于最后 time-analysis 源码修正（21:46:37）。见 `logs/evidence-r4-complete-relink.log`；先前完整 UI 包落后于 infrastructure 的版本缺口已在独立构建关闭。新的原生旅程仍未执行，不把重新链接当作截图验收。

隔离草稿的 Release Qt UI CTest 使用原有 300 秒上限通过，耗时 57.93 秒；QtTest 明细为 94 passed、0 failed、0 skipped、57.608 秒。见 `logs/evidence-r4-ui-ctest-300.log` 与 clone 的 `out/vs2026-qt611-release/ui-test-results-release.txt`。该 offscreen QWidget 回退检查不证明 GPU 或规定显示器的原生效果，也不替代 canonical Debug 的完整回归。

增加独立可手算的 512×64 无插值 ROI：四分之一格为 0 dB，其余 -100 dB，去均值后正值均值 75、off fraction .75、时间门限 28.125、频率门限 14.0625；活动边缘仍保持预期物理格。domain 从 16 增至 17 组。真实临时 IQ 的 ROI 回退记录另验证保存两门限及数值域。

融合 fixture 保留 parent 原始字段，新增独立目录 append/end-marker/recover 的完整 DTO 精确回环，并确认嵌套 parent 的完整长 seed 保留。该 fixture 不执行模型，不当作效果证据。原生截图探针草稿新增两项实际 ROI 门限存在的检查，仍使用原有截图入口；尚未运行这个新原生探针。

独立源由本仓库 249 个源/构建输入复制，经过 NUL 分隔 UTF-8 Git 清单与逐字节核对；不依赖外部业务源码。第一次清单中三个 quoted Unicode docs 路径被错误纳入，均为 docs 且未复制；随后严格排除 docs 并核对全部实际源文件。工具路径按忽略的本机 CMakeUserPresets 显式提供。配置、构建均使用复制的本项目 scripts/build.ps1。

独立 Release 构建通过，相关 CTest 2/2 通过（1.26 秒）；直接运行输出 `Detection refinement tests: 17 groups passed` 和 `Time pipeline checks passed`。UI 中的门限显示与 Coarse 单位修正已编译通过，但新原生截图、全套 UI 和 canonical 集成仍未验证。日志位于本节点 `logs/evidence-r4-*.log`。

R3 数值矩阵保持原 freeze；这份草稿只补证据，不从结果反选模型、参数或容差。待冻结运行阶段完成、用户决定项明确后再按本节点顺序集成、自检、复审及本地提交。不能提前进入 M5。
