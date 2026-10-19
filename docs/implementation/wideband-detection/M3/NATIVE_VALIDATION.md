# M3 原生验收与原型对照

最终截图入口为 `scripts/screenshot.ps1 -Configuration Debug -WidebandDetection -ProductionModel`。中间产物位于 `out/tasks/wideband-detection/M3/diagnostics/native-geometry-final/`；归档 PNG 与原始报告保留实际字节和 SHA，不改变物理截图尺寸。

实际第二连接屏为 Redmi 27 NU / DISPLAY6，物理 3840×2160、实际 150% DPI、逻辑 2560×1440、全屏。图谱使用实际 QRhi Direct3D11、非 CPU 驱动和已上传纹理。推理另由 NVIDIA GeForce RTX 5060 Laptop GPU 执行，图谱 GPU 不作为模型前向证明。

25 个场景通过。原有 23 个场景继续验证真实 TorchSig IQ、四条原始标签、一条手动标记、明确系统夹具、常驻卡、任务证据、保存重开、ROI、三手柄、三列证据、原始字段折叠及单段确认。新增场景 23 展示原型常驻卡中的真实模型预检/运行；场景 24 展示实际三条模型频率观察和窗口/分数证据。来源集合发布后，该真实文件有 4 条标签、1 条手动、3 条系统夹具、3 条正式模型观察，共 11 条记录。

原型 `running-frozen.png`、`review-evidence.png` 与新增原生场景按区域/动作对照。三来源颜色、固定动作、范围/物理时间、互斥策略、结果来源、未知活动时间和复核语义保持。导航/辅助图继续采用批准规划保留的 A1.4.3 原生比例，真实 IQ 图谱和模型数值不复制原型合成图。M3 没有时间活动段和真实精修曲线，证据中明确保持未定位，不画合成曲线冒充模型效果。

首次新增模型场景失败于截图辅助函数把所有来源格式固定为 SigMF；修正为按实际来源验证，旧 23 个场景仍要求原 SigMF 格式，新模型要求 `detection-v1`。目视检查又发现未知时间的禁用几何框残留上条标签数值；修复后四个框为空占位，原生断言验证禁用、无草稿、无旧坐标。最终截图分别保存到新目录，初次失败及旧成功产物仍留在 M3 diagnostics/logs，不覆盖为新结论。

`modelInferenceVerified=true` 仅表示此真实 TorchSig 文件经正式模型前向及来源发布；`timeEventsVerified=false`、`a16AcceptanceVerified=false` 继续保留。M4 负责时间精修/效果和算法段到 DDC，M5 完成全部旅程与部署验收。
