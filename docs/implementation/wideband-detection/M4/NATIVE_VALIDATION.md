# M4 原生旅程与原型对照

通过项目既有入口 `scripts/screenshot.ps1 -Configuration Debug -WidebandDetection -ProductionModel`，第 5 次完整原生探针通过 34/34 场景。实际第二连接屏为 Redmi 27 NU / DISPLAY6，3840×2160 物理、150% DPI、2560×1440 逻辑、全屏；报告记录连接序号 2。图谱为 QRhi D3D11 / AMD Radeon 880M，有实际纹理上传；模型由独立 NVIDIA TensorRT worker 执行，二者证据分别保存。

这份截图对应长 ID / 非默认回退参数修正之前的源码身份。原型界面未因此修改；最终节点仍需用最新可执行文件重跑原生探针。`native-report.json` 与 34 张 PNG 保留原始字节，不能把旧截图标为最终新版本。

| 原型行为 | 原生证据 |
|---|---|
| P01–P04、I01–I04：同一常驻卡、四范围、ROI 确认/取消 | 既有场景 01–10，保留 M2 对照与 Qt 配置/ROI 检查 |
| P05、P17、I18–I21：辅助检测证据、来源/原始字段、已保存测量 | 场景 17–19、24；实际 sample 轨迹来自已保存线性功率，不重新读文件或绘制原型合成曲线 |
| P08、I16–I17：真实任务阶段、计数与冻结快照 | 场景 23；阶段与精修计数由实际任务回报，fixture 与真实模型来源分开 |
| P09–P12：接受不复制对象、复核与修订 | 场景 25、29；ID/数量不变，人工几何修订保留不可变算法来源 |
| P13、I13–I15：ROI 生效参数及 noSplit | 场景 31–33；边缘比例 .75、平滑 2 ms、min on/off 禁用；保存实际 kernel=11、durationParametersApplied=false |
| P14、I22：确认算法活动到通道、来源快照 | 场景 26–30；真实 CPU DDC 产生 16384 样本，有限正能量与 SHA 保存；保存重开按快照值核对，来源修订后标 Changed，不静默重算 |
| I23–I24：可调整布局、固定动作及规定显示器 | 场景 11–16；沿用 A1.4.3 比例与获批 M0-P 布局/交互，不复制合成图谱数值 |

原型 `review-evidence.png`、`channel-source.png`、`channel-source-changed.png` 与原生 24、26–30 对照；`running-frozen.png` 与 23 对照；ROI/策略区域与 31–33 对照。目视检查了原生 24 与 31：来源色、常驻卡、三图、底部记录、固定运行区及有效参数矩阵保持获批形式。全部 S0–S9/I01–I24 的最终 A16 综合验收属于 M5，本报告仍保留 `a16AcceptanceVerified=false`。

前四次探针失败和修正记录保存在节点 diagnostics/logs：选择后未显示检测证据模式、DDC 后等待了隐藏的宽带图、保存重开错误比较共享指针地址、复选框点击到空白区。均按真实可见控件与值语义修正探针，未添加新的截图框架。最终成功结果不能覆盖这些失败历史。

真实 TorchSig 文件上的模型前向、sample/ROI 测量与 DDC 旅程不证明 G/T 总体精度、实采 RF 效果或 GPU DSP。M4 效果指标由独立完整矩阵报告，不能以这些截图代替。
