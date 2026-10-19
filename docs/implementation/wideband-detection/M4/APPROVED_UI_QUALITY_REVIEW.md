# M4 已批准 UI 变更质量复审

结论：**QUALITY_PASS_APPROVED_UI（仅三个 canonical 文件中的获批增量及已结束的定向场景证据）**。未发现新增质量阻断。SCN 无效裕量控件的禁用、已保存值保留和作用说明符合批准决定；非 SCN 原启用矩阵保持。480 秒只改变 CTest 终止保护。**本结论不代表完整 UI、native 或 M4 节点通过。**

2026-10-14（Asia/Shanghai）。用户在两份决定记录中明确批准建议 A，两项决定已获答复，不再记作待确认。本审查在 `APPROVED_UI_SPEC_REVIEW.md` 的 SPEC_PASS_APPROVED_UI 后只读 `CMakeLists.txt`、`ui/detection/detection_card.cpp`、`tests/ui_tests.cpp` 及已完成的定向日志，仅新增本报告；没有修改源码、freeze、原型，未构建、运行测试/GPU、停止进程、提交或进入 M5。没有读取进行中的隔离 Debug All 日志判定通过，也不扩展旧全 M4 审计。

## 实现与回归核对

`detection_card.cpp:814–815` 用 profile backend 的 enum 判定 SCN，条件为 `!scn && strategy != Coarse`。它没有依赖显示名称、特定字符串或当前按钮，能覆盖历史 request 中的 None；非 SCN 仍严格沿用原来的 `strategy != Coarse`。其他 branch 参数、split 条件、布局及运行/取消动作未因本项改变。

禁用操作只调用 setEnabled，没有向 stored margin 赋默认值。`configure:1099–1121` 复制历史 request，`loadControls:599–625` 在 updating 状态下撤销草稿并加载已存储参数，`switchProfile:587–597` 保存当前 profile 配置后恢复目标 profile 配置。因此禁用不会丢失历史 SCN 值，也不会把它混入切回的非 SCN 配置。

`refreshTiming:759–775` 明确区分固定 CNR/频率收边 3 dB 与 Coarse/粗回退 6 dB，说明不可调且保存值保留；SCN ROI 帮助改为自适应门限及固定裕量。没有再把可调 dB 值说成 Sample/ROI 时间门限，也没有把这些 dB 说明写成 sample 线性功率或新增算法。非 SCN 的原文与规则保持。审查的变更行未见超过80字符或行尾空白；未重跑格式工具。

新增 `widebandConfigurationCommittedDrafts` 断言 `ui_tests.cpp:4302–4339` 是实际控件及 request 行为回归：先在非 SCN Sample 通过 Enter 提交 margin=9；载入 SCN margin=11.25 快照，逐一切换 Sample/ROI/Coarse 后核对禁用且 request 值不变；再载入历史 None 继续核对。切回非 SCN 夹具时检查 margin 重新启用、恢复9且 score仍为.8。历史 None 只是配置载入边界，没有新增 None 按钮或承诺它可启动检测。原来的输入草稿、Enter提交、滚轮及文件切换后的 score 检查没有删减。

这项测试直接约束异常 profile 的禁用及 switchback 值隔离，能够发现误清零或错误扩展到非 SCN 的回归。实际非 SCN 执行 profile 是 pipeline fixture；FFSCN/实验 profile 的条件保持来自同一个 enum predicate 的静态核对，不冒称每个生产 profile 都单独执行了本场景。

`CMakeLists.txt:345–351` 的批准变化仅为 `signal_studio_ui` 的 TIMEOUT300→480；runner仍为 `cmake/run_ui_test.cmake`，目标和日志参数相同，环境仍为 offscreen及scale1.5。其他已有 timeout、真实用例、断言和DPI没有借此变化。480秒是终止保护，不是交互性能目标，也不是一个新测试通过结果。HEAD diff中其他 M4测试目标/阶段显示是既有增量，不纳入这次批准变更的质量结论。

## 已结束证据与身份

`out/tasks/wideband-detection/M4/logs/approved-scn-ui-case.txt` 显示 Debug Qt 6.11.1 的 initTestCase、一个 `widebandConfigurationCommittedDrafts` 业务场景和 cleanupTestCase 共 **3 passed、0 failed、1931 ms**。这是一个场景及初始化/清理通过，不是三个独立业务场景或全套 UI通过。日志的 QRhi不受支持提示与此功能结果并存；该软件/offscreen证据不证明 GPU、规定第二显示器或原生截图验收。

| 当前文件/日志 | 原始字节 SHA-256 |
|---|---|
| `CMakeLists.txt` | `d37722d664c4c2c7518257c378071cce3272535b61c6d70cf9a3f112aac036cc` |
| `ui/detection/detection_card.cpp` | `064a0e300d53f3ebc3d68ccec177c317e0a35887f3e516e5ab83bd55a5f820a6` |
| `tests/ui_tests.cpp` | `c504fc656ee42bc9dddb95dceb6dcbd01bf2f2a027c1ec42beaf85d6e9a803ff` |
| `approved-scn-ui-case.txt` | `fe021479fe0433d305c40181b1ae3651fe1f0c5ab73248ed09d6e354a02fc556` |

三个源码 SHA与本次 SPEC记录相同。读取 R3 freeze后确认三个文件都不在其72项 inputs中；freeze自身 SHA仍为 `7e2d9fa220e766be60fd5ece80aed79c66749e09ae2821b22baff4269ab6ad8a`。SPEC已独立记录72/72匹配，本次QUALITY没有重复计算全部输入或308数据资产，不把这三个UI文件的核对扩大成整仓身份锁定。

SCN控件例外与480秒保护在本批准范围可通过质量复审；旧300秒超时历史保留，新预算下完整隔离Debug结果只能在执行结束后另行读取。正式矩阵仍运行，evidence/IO草稿尚未canonical集成，新native及最终节点证据不由本报告替代。此处不宣称效果门槛、全部回归或M4完成，不授权提交或进入M5。
