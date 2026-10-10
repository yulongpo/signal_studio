# Import A1.2 / Brand v2 Milestones

| Node | Status | Gate / Evidence |
|---|---|---|
| M0 | Complete | BASELINE_AUDIT.md；干净工作树、fetch、基线审计 |
| M1 | Complete | Debug build；CTest 6/6 (243.80s)；144组格式/读取/v4往返；旧CI16、DDC、谱分析、UI回归 |
| M2 | Pending | 原型三页布局、单位提交、真实四图预览、停止/前缀 Qt 自动交互 |
| M3 | Pending | 共享品牌资源/ICO/全局入口/关于/资源检查与截图 |
| M4 | Pending | SigMF/来源/模板/队列隔离/取消/重试/去重测试 |
| M5 | Pending | Debug/Release/CTest、独立包、GPU/软件、九张截图、执行报告 |

每节点 Gate 通过后更新自查与证据并本地提交。所有节点完成后正常推送当前分支，不强制同步。UI 按 A1.2 信息架构和视觉尺寸还原，主窗口仍遵守 A1.4.3。未实现项不能用模拟结果替代。

## M1 自查

完整覆盖六编码、实/复、字节序、三种复数布局、通道交织/分块、选中通道、头尾及完整帧。格式 JSON 严格验证；v4 字段往返，v1/2/3 回填旧 CI16。旧 CI16 归一化数值逐样本相等，真实实数 PSD 单边功率积分匹配 0.125。读取块按完整帧、32MiB 片段预算、导航<=2048；非4字节帧停止/前缀重读通过。缓存包含格式/代次/前缀；实数 DDC 在业务与处理器层拒绝。

命令：`scripts/build.ps1 -Configuration Debug -Action Configure`、`-Action Build -Jobs 8`、`ctest --preset local-debug --output-on-failure`。原生日志见 M1-ctest.txt / M1-ui.txt。M2 开始前已完成旧功能回归。
