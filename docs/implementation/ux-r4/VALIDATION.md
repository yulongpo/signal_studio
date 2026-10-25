# UX-R4 验证

## 最终结果

在本次唯一实际 4K/150% 屏幕 Redmi 27 NU（连接 1，`\\.\DISPLAY6`）完成原生验收；
物理 3840×2160、Qt 2560×1440、DPR 1.5。设备身份、像素尺寸、实际字段文本、侧栏宽度、
控件矩形、renderer、上传/绘制统计见四组 report.json。未用虚拟超大窗口替代。

| 配置 | 原生结果 | 证据 |
|---|---|---|
| Release D3D11 / RTX 5060 Laptop | 21 主窗口场景 + 详情，通过 | [report](evidence/release-gpu/report.json) |
| Release QWidget / QPainter | 21 主窗口场景 + 详情，通过；带分步日志复查 20.62 s | [report](evidence/release-software/report.json) |
| Debug D3D11 / RTX 5060 Laptop | 21 主窗口场景 + 详情，通过；带分步日志复查 86.71 s | [report](evidence/debug-gpu/report.json) |
| Debug QWidget / QPainter | 21 主窗口场景 + 详情，通过 | [report](evidence/debug-software/report.json) |

每组使用实际 20 MS/s / 400000 CF32 样本的图谱、原有真实标签和明确标注的交互夹具。
最后一个大频率场景是 **999.999 GHz 合成元数据的 UI 压力夹具**，不证明此频率的 RF 分析精度。
每组均通过新侧栏按钮创建真实通道，保留来源快照，并执行 512 输出样本的真实 CPU DDC。
未在本轮重跑 TensorRT 效果矩阵，不把显示 GPU 或合成标记算作模型推理证据。

## 场景和断言范围

- 默认、超长中英文名称、320/390/650 逻辑像素侧栏、隐藏/恢复；主操作中心命中检查、
  完整可见矩形、常用按钮至少 28 px、标签 heightForWidth 与两两碰撞检查。
- 上一/下一段不缩放；定位改变视图；表、工程树、图谱实际点击共享当前对象。
  原始标签与算法夹具有重叠，图点击测试通过真实图层复选框暂时隐藏重叠标签，
  既有自动化另外覆盖重叠命中、多选锚点和批量动作。
- Enter 提交范围、撤销恢复；拆分/删除当前段及撤销；未定位时间不制造活动段；
  多选明确当前对象、创建入口禁用；无选择/缺失来源保持无效上下文保护。
  缺失 sourceSet 的非法测试记录按既有 reconcile 规则撤销选择，不修改领域规则来保留无效可编辑对象。
- 默认摘要不显示完整 ID/SHA/JSON；详情复制包含原始 JSON 尾部、完整来源路径/哈希、
  引用和修订，复制 37072 字符且 encodeAnnotation 前后相等。
- 辅助证据、PSD、时频、瀑布、换色表、24 个新增密集标记、ROI 预览/取消、控制柄；
  悬停检查不增加谱功率计算次数。保存重开 .ssproj 后保留通道来源快照。

原生输入由真实 Windows Qt QWindow/QTest 及已有可见控件助手驱动；不是 offscreen 测试。
截图使用可见原生窗口上的 QWidget/QRhi 回读，详情为实际非模态对话框 client crop。
源码中每场检查窗口仍全屏、位于选定屏幕且 DPR 正确。

## 构建与精简回归

Debug/Release 均经 `scripts/build.ps1` 构建 SignalStudio、SignalStudioUiCapture、
SignalStudioUxTests 和 SignalStudioUiTests。最终应用源码的相关 CTest 每配置 12/12：

| 范围 | 每配置 CTest 结果 | 说明 |
|---|---|---|
| UX charts/layout/popovers/annotation | 4/4 | 含新证据完整复制、当前对象/动作状态；仍覆盖原溢出/草稿/ROI/窄带页 |
| UI annotations 定向覆盖 | 1/1 | 11 业务用例；树/图/表、多段、颜色、几何、删除、草稿、TorchSig、worker 夹具保存重开 |
| 参数/标记/标记持久化/标签/WAV/工程文件/工程树 | 7/7 | 标签各配置 32 pass，4 个符号链接场景因主机权限跳过；不隐瞒跳过 |

五组 Qt UI 日志每配置共 27 个业务函数（PASS 总数包含各程序 init/cleanup，不作为新增功能数）。
offscreen CTest 证明交互和状态，不证明硬件显示。真实硬件显示由上表四组原生报告单独证明。
结果并非一次性全库矩阵；本轮只运行受影响范围。详细 CTest 和 Qt 日志在 [evidence/logs](evidence/logs/)。

`scripts/verify-package.ps1` 在 Debug/Release 两个 `_bin` 目录分别验证 GPU 与软件独立启动，
共 4/4；system-only PATH、实际 Windows 平台、CompactHeader 默认行为和部署运行库均保留。
详见 [evidence/package](evidence/package/)。没有进行额外批量 SHA 计算或模型/DLL 指纹清单。

## 失败、复查和限制

1. 初次新增删除按钮在挂载父布局前查找自身导致空指针，已修复并经最终四组运行覆盖。
2. 初次通道脚本未等待非模态确认；图点击又命中重叠真实标签。已分别改为等待实际确认、
   通过实际图层开关消除歧义；既有重叠选择回归仍保留。
3. Windows grabWindow 后续图片返回旧 DWM 画面，原后续基线图片已排除。基线仅保留初始默认帧，
   最终改用项目既有 QWidget/QRhi 回读。不能将早期通过 JSON 当作有效视觉通过。
4. 软件等待曾误用 GPU 帧计数，已改为软件显示就绪条件。一次后续 Release 软件运行出现约
   202.54 s 的两帧输出间隔及高 CPU/不响应观察，之后自行继续并完成。带日志复查 20.62 s 完成全部场景，
   未复现；原因未定位，**不宣称软件性能稳定性已验收**。保留 [时戳诊断](evidence/diagnostics/software-latency.json)。
   原生 runner 增加 300 s 超时、进程结果和步骤日志，异常不会无限等待。
5. 大频率首次超出既有 ±1 THz 标记合同，被拒绝；已使用合同内元数据夹具，不修改领域边界。
6. 原型 JavaScript 语法检查通过；Computer Use 因无法可靠识别浏览器 URL 停止，未继续浏览器自动化。
   不声称原型浏览器交互已实测。交互原型可用文件交付；实际 Qt 功能由上述原生点击证明。

既有 Ubuntu、设备输入、现场 RF 精度和模型识别/解调限制不因本轮 UI 优化改变。
独立审核及发布结果以提交外 `INDEPENDENT_REVIEW.md`、`PUBLISH_RECORD.json` 为准。
