# UX-R3 G3 坐标轴布局

2026-10-25 反馈修复更新：当前默认改为 `CompactHeader`，X/Y 轴名、单位和原点说明放入现有标题栏；紧凑模式只共享主辅图左侧刻度预算。A/B/C 的枚举值及可选行为保留。以下 G3 冻结记录和原验收数字属于历史实现；当前实现、面积比较及验证见 [feedback-01/DELIVERY.md](feedback-01/DELIVERY.md)。

本节点在 `feature/ux-r3-optimization` 共享工作区增量实现，实施写集为 `ui/charts/plot_widget.{h,cpp}`、`ui/charts/axis_layout.h`、`tests/ux_r3_axis_tests.cpp` 和本文；随后按单独分工新增 `tests/ux_r3_capture.h`。CMake、native header include、CLI 和既有 UI 测试适配由主 Agent 集成；没有改写 DSP 或用户原有未跟踪内容，没有提交、推送或切换分支。

## 实现冻结

冻结时间为 **2026-10-23 20:49:00 +08:00**。从本次冻结起，实施 Agent 只更新本文，不再修改轴测量、绘制、交互、轴单测或 native runner，供主 Agent 进行下一轮统一构建。冻结时读取到的文件最后写入时间如下；这是本机文件时间，不代替 Git 或构建身份。

| 文件 | 最后写入时间（北京时间） |
|---|---|
| `ui/charts/axis_layout.h` | 2026-10-23 20:21:36 |
| `ui/charts/plot_widget.h` | 2026-10-23 20:14:16 |
| `ui/charts/plot_widget.cpp` | 2026-10-23 20:18:22 |
| `tests/ux_r3_capture.h` | 2026-10-23 20:36:47 |

## 实现与预算

旧版主图、辅助图固定为 `QRectF(88,12,width-88-right,height-46)`；导航图保持原几何。新版 `PlotWidget::plotRect()` 委托 header-only `axis_layout`，仍是绘制与交互的统一入口。

`measuredAxes()` 先生成实际要画的全部 X/Y 刻度和物理标题，再用 `QFontMetricsF(font(), this)` 测量。测量包含 advance、字形左右越界、行高、负数、科学计数法；不使用字符数估计宽度，不给长刻度设置 64/88 px 上限。字体、DPI、DPR、窗口尺寸、视区、来源单位和模式参与缓存键，普通悬停不重复格式化与字体测量。距离均为逻辑像素，不再次乘 DPR。

同一窗口、同一 `Session` 的主图与辅助图自动共享最大 left/top/bottom 预算；即使一图隐藏，仍保留对齐基准。各自的刻度、单位和字体独立测量。导航图不参与预算。主图色标仍保留原来的 70 px 右边距，无色标及辅助图为 22 px。

左边距由外侧 8 px、可选旋转标题宽度、实际最宽 Y 刻度及图框间 7 px 组成，至少保留 36 px 轴操作区。36 px 是命中区域下限，不是所有图的固定 left。48–64 px 仅是普通短刻度的评估目标；实际数值由字体和数据决定。X 刻度带位于末端 Y 刻度文字下方，X 标题另有行高和间隔；顶置单位/标题也与最高 Y 刻度分开。字体变大时最小控件高度同步增大，保留至少 16 px 绘图区。

## A/B/C

| 模式 | `AxisTitleMode` | 显示 | 横向成本 |
|---|---|---|---|
| A（默认候选） | `UnitAtTop` | Y 轴顶部独立一行单位，保留 Δ/基带语义 | 刻度预算 |
| B | `Header` | 图表内部顶部标题带承载完整物理量和单位，按宽度换行 | 刻度预算 |
| C | `Rotated` | 左侧传统旋转完整标题；短图按可用高度换行 | 刻度与旋转标题预算 |

B 的标题带在 `PlotWidget` 内部，由同一 QPainter 覆盖层绘制；无需改动外部 `ChartControlStrip`，不占用外部窗口标题栏。A 不把功率单位改成通用 dB：整数归一化 PSD 为 `dBFS/Hz`，未归一化整数为 `dB(ADC^2/Hz)`，浮点为 `dB(unit^2/Hz)`；波形为 ADC 计数/原始幅度/演示单位。频率按 Hz/kHz/MHz/GHz、时间按 s/ms/us 选择；大原点小范围继续保留相对刻度及 F₀/T₀ 提示。

默认采用 A。2026-10-24 Release 在实际 Redmi 27 NU / DISPLAY6（连接 1、3840×2160、150%）上的硬件与软件矩阵各 24/24 场景通过，代表截图已目视检查。A/B 的横向预算相同；A 的单位提示更短，B 保留完整物理标题，C 可选。

Debug 随后在相同实际屏幕上硬件/软件矩阵也各 24/24 通过；四种配置合计 96 个原生场景。Debug 最终硬件报告位于 `G7/native-axis-debug-v2/`，软件位于 `G7/native-axis-debug-software/`，与早期被中断的 `native-axis-debug/` 分开。

## 相对旧版的横向与纵向成本

横向收益与纵向成本必须同时报告。外部给图表控件分配的空间、控件内部文字占用、最终纯数据绘图区是三个不同的量。测试继续约束外部分配空间，不表示新增文字预算不会占用纯数据区；不能将宽度增加直接写成有效面积全面增加。

设同一场景下控件逻辑尺寸为 `W × H`、右边距为 `R`，共享后预算为 `L/T/B`（left/top/bottom）：

| 项目 | 旧源码基准 | 新布局 | 新版相对旧版 |
|---|---|---|---|
| 数据区宽度 | `W - 88 - R` | `W - L - R` | 宽度收益 `88 - L`；负值表示更窄 |
| 数据区高度 | `H - 46` | `H - T - B` | 纵向文字增量 `T + B - 46`；高度收益为其相反数 |
| 数据区面积 | `(W-88-R) × (H-46)` | `(W-L-R) × (H-T-B)` | 在同一 `W/H/R` 下分别计算，不只比较 left |

这里的旧源码纵向总扣量 **46 px** 来自冻结前 `plotRect()` 的 `QRectF(88, 12, ..., height()-46)`：top 为 12 px、bottom 为 34 px。不能把起点 12 再加到 height 的扣量上而得到 58。

主 Agent 当前维护的旧 UI 回归采用另一项明确的**分配空间归一化参考**：

```text
plot.height + budget.top + budget.bottom - 46 >= 839
```

在没有触发最小 1 px 数据区保护时，左侧等于 `H - 46`，因而不受 A/B/C 的内部文字预算变化影响，保留原外部分配空间的门槛。本文保留这条测试约定，不修改其门槛；与旧源码纯数据区 `H - 46` 的分配门槛一致。验收表须同时列出相对源码的纵向成本 `T+B-46` 和同一分配门槛下的文字预算成本，避免混用这两个基准。Y 轴交互测试改用实际 `plot.left()/2`，同样是在动态轴区内取点，保持原先测试 Y 轴滚轮的物理语义。

三模式的成本来源如下；下列为实现公式，**不是实屏测量值**。设 `m` 为实际测得并向上取整的字体行高，`hA/hB` 为单位/完整标题实际换行后的高度，`w` 为最宽 Y 刻度 ink 宽度，`rC` 为旋转标题带宽度（含间隔）。两图最终再对 left/top/bottom 分别取最大值。

| 模式 | 本图 left 来源 | 本图 top 来源 | bottom 来源 | 需实测的代价 |
|---|---|---|---|---|
| A | `ceil(max(36, 15+w))` | `ceil(max(12, 8+hA+m/2))` | `ceil(m/2)+2m+11` | 顶置单位增加纵向占用，减少旋转标题的横向占用 |
| B | `ceil(max(36, 15+w))` | `ceil(max(12, 8+hB+m/2))` | 同 A | 标题不换行时接近 A；换行增加 top |
| C | `ceil(max(36, 15+rC+w))` | `ceil(max(12, 8+m/2))` | 同 A | 省去顶部标题行，但左侧增加旋转标题带；短图的旋转标题换行会进一步增宽 |

最终测量必须从同一配置、renderer、字体、场景和外部尺寸的实际截图/JSON 取得 `L/T/B` 与有效数据区宽高。`axisLayoutStatistics()` 的 plot.y 是 `T`，`B = logicalWidgetSize.height - plot.y - plot.height`；`renderBeforeGestures.charts` 提供实际控件尺寸，`mainAxes/auxiliaryAxes` 提供轴数据。不得用不同配置或不同外部分配尺寸的裸宽高差代替预算比较。

2026-10-24 Release 实测如下，硬件 D3D11（NVIDIA GeForce RTX 5060 Laptop GPU）与 QPainter 软件的几何结果一致。每行汇总该模式下 waveform/PSD × TF/Waterfall 四个场景；完整逐场景数据保留在 `G7/native-axis-release*/diagnostics/*/ux-r3-*/ux-r3-report.json`，计算汇总为 `G7/diagnostics/axis-release-measurements.json`。

| 模式/刻度 | 共享左边距 L | 横向收益 `88-L` | 纵向成本 `T+B-46` | 同尺寸纯数据面积变化（主/辅助合并范围） |
|---|---|---|---|---|
| A/B 短 | 40–42 px | +46–48 px | +37 px | -27.76% 至 -1.71% |
| A/B 长 | 46–62 px | +26–42 px | +37 px | -28.56% 至 -2.03% |
| C 短 | 78–80 px | +8–10 px | +21 px | -16.42% 至 -1.90% |
| C 长 | 84–100 px | -12 至 +4 px | +21 px | -17.37% 至 -2.23% |

这次改动改善了横向利用率，并为原先共用紧凑空间的单位、端点刻度和 X 标题分配互不遮挡的独立文字带。它**没有增加总数据面积**：辅助图区较短，新增纵向预算的相对代价最大；独立重算 A/B 主图面积减少 1.708%–2.904%、辅助图减少 27.68%–28.56%；C 主图减少 1.896%–3.116%、辅助图减少 16.327%–17.367%。原生 JSON 保留部件尺寸与纯数据 plotRect，外部分配测试不是纯绘图区高度下限。保留这项实测成本，不把宽度收益写成总体面积提升。长刻度 C 超过原 88 px 时优先保证文字完整。

硬件 24 场景最大游标往返误差 0.00875 逻辑像素，ROI 最大误差 0.00745，标记角点最大误差 0.594；全部通过保留的 1.1/2.1 像素上限、文字 ink 边界和两图左缘对齐检查。软件路径也执行相同物理手势与测量断言。PNG 的原生像素为 3840×2160，无 DPI 覆盖。

每个 renderer 的 24 个原生场景不得合并为一个“典型值”。可以按模式、短/长刻度汇总 waveform/PSD × TF/Waterfall 的最小值和最大值，但应保留逐场景 JSON 链接与失败记录。A/B/C 比较同时列出 top、bottom、宽度收益、纵向成本及数据区面积；长刻度超出 88 px 时允许负的宽度收益，不以 48–64 px 作为硬性通过条件。

## 原生截图与测量接口

```cpp
auto* main = window.findChild<signalstudio::PlotWidget*>("mainPlot");
auto* aux = window.findChild<signalstudio::PlotWidget*>("auxPlot");
using Mode = signalstudio::PlotWidget::AxisTitleMode;
main->setAxisTitleMode(Mode::UnitAtTop);  // A；同 Session/窗口的 aux 一起切换
main->setAxisTitleMode(Mode::Header);     // B
main->setAxisTitleMode(Mode::Rotated);    // C
const QJsonObject mainAxes = main->axisLayoutStatistics();
const QJsonObject auxAxes = aux->axisLayoutStatistics();
```

`setAxisTitleMode` 为 `Q_INVOKABLE`，也暴露 `axisTitleMode` Qt 属性和 `Q_ENUM`。模式仅是本次图表展示状态，不写工程/QSettings；切换模式取消正在进行的手势并保留原有回滚语义，不改物理视区。调用时应先创建主图和辅助图，再设置任一图的模式。

`axisLayoutStatistics()` 为 `Q_INVOKABLE`，返回 mode（A/B/C）、plot/header/X 刻度带/X 标题带、localLeft/sharedLeft、最宽刻度、行高、相对旧 88 px 的 leftGainFrom88、全部实际刻度、完整物理标题/单位、原点、字体、逻辑 DPI 与 DPR。`renderStatistics()["axisLayout"]` 也包含同一对象，既有渲染报告可直接采集。导航图的 `plot` 仍报告原导航几何，其余轴字段不作为双图比较对象。

## 手势和渲染

ROI、标记框/控制点、鼠标命中、光标、滚轮和轴拖动继续从同一个 `plotRect()` 映射，不使用旧 88 px 常量。真实整数时间原点和相对样本偏移、时频/瀑布的物理轴互换均保持。

一次拖动或滚轮批次开始时固定两图当前几何预算，防止数值位数变化导致轴位置移动；完成/取消后释放并重新测量。字体/DPI/尺寸变化取消手势并刷新两图。辅助曲线缓存键新增 plot left/top，避免绘图区平移后复用旧像素顶点。QRhi 热图、曲线顶点和 QPainter 软件回退继续使用既有路径。

## 自检与待统一执行

已完成源码路径核对与静态检查；实施 Agent 未自行构建或运行测试，遵守本次“构建由主 Agent 统一”的分工。主 Agent 已接入 `SignalStudioUxR3AxisTests` / `signal_studio_ux_r3_axes`、native header 与 `--ux-r3 <path> [--software-renderer]` CLI。

已核对本轮完整 Debug 构建/CTest 日志 `out/tasks/wideband-detection/UX-R3/G7/logs/Debug-full-20261023-203805-321.txt`：`signal_studio_ux_r3_axes` 为 **Passed，1.00 s**。同一轮 `signal_studio_ui` 保留了 `workspaceLayout()` 的旧纯绘图区高度断言失败；主 Agent 现已按上述分配空间口径适配，并把旧 `x=40` 的 Y 轴轮事件位置改为实际 `plot.left()/2`。适配后的整轮回归尚待下一轮构建核验，不能将旧失败删除或称作整轮全过。

`tests/ux_r3_axis_tests.cpp` 覆盖：

- 文字 ink 与 margin、顶置与旋转成本、X/Y 文字带互不相交。
- 大字体、负数、长科学刻度超出 88 px 时仍扩展预算。
- 实际两 `PlotWidget` 对齐、不同字体共享预算、A/B/C 不改视区、色标右边界。
- GHz/时间换轴、浮点 PSD 与波形真实来源单位。
- A/B/C × 时频/瀑布的物理端点映射及 X/Y 命中区。
- 辅助 Y 滚轮不改变时间/频率、批次固定几何、轴拖动 Esc 恢复。

上述单测属于合成/几何与交互夹具，不是真实 IQ 精度或原生 GPU 验收。主 Agent 另以实际 IQ 在指定 4K/150% 屏幕完成四配置各 24/24 的 A/B/C 原生矩阵，覆盖辅助 Y 隔离、X/Y 滚轮和拖动、ROI、标记角点/撤销及物理游标；实测宽度、裁剪边界和坐标误差见本文前述结果。完整 UI 回归另覆盖框选、多选、驻留游标、最大化和参数提交，逐轮结果见 VALIDATION。
