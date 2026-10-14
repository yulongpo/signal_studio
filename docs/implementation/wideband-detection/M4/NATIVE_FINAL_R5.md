# M4 最终原生旅程与原型对照

本次使用已集成 r5/IO-r2 和批准参数规则的主目录 Debug 包，通过既有 scripts/screenshot.ps1 -WidebandDetection -ProductionModel 完成 34/34 场景。完整报告、34个原始 PNG 和逐项 SHA 位于 native-final-r5/；较早截图继续保留，不改成新版证据。

实际连接屏2：Redmi 27 NU / DISPLAY6，3840×2160物理、150% DPI、2560×1440逻辑，全屏。三个图提交实际 QRhi D3D11 帧，驱动 AMD Radeon 880M，主图有59次纹理上传；模型推理另由 NVIDIA GeForce RTX 5060 Laptop GPU / TensorRT10.11.0.33执行。

场景24展示保存的 Sample 实测功率、门限、窗口分数和时间依据；场景31–33验证批准的ROI参数规则及保存重开。ROI实际保存 positive-db-residual-mean-v1 数值域、positiveMean=7.9950465363、timeThreshold=2.6590276042、frequencyThreshold=1.3295138021；边缘比例.75、平滑2ms、kernel11，关闭拆分后 durationParametersApplied=false。没有把这组ROI自适应残差门限描述为Sample线性噪底。

场景25–30验证同对象接受、确认活动→通道参数→真实CPU DDC→保存重开→人工修订。DDC产生16384样本，有限正能量，SHA-256为29e6aa2b970632e10eb283329c08424190eee0ecfe1942986327f4ad4e8f094e；来源修订后标Changed，原通道数据和快照不静默更新。

主 Agent 目视对照了获批 M0-P 的 review-evidence.png、roi-confirm.png 与本次24/31及既有ROI场景。常驻卡、三图结构、三来源色、统一记录、固定运行区、互斥策略及确认手势保持；真实数据和测量替换原型合成内容，原生导航/辅助图比例沿用规划保留的A1.4.3。SCN无效裕量禁用是用户明确批准的例外，完整Qt场景验证该例外及切回profile后保留存储值。

报告 modelInferenceVerified=true、timeEventsVerified=true。a16AcceptanceVerified=false继续保留，全部S/I综合映射与部署验收在M5执行；此旅程不代替全数据集L3/L4或实采RF效果。
