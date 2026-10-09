# 图谱一致化与时域幅值验收补充

日期：2026-10-09。此记录覆盖宽带/窄带对应图谱的共同交互、ADC 计数等效波形、75% 自动适配、峰值包络和图谱 GPU 绘制。机器报告见 [`chart-rendering-followup.json`](chart-rendering-followup.json)。

窄带时域值以 ADC 计数等效单位显示，不再归一化；没有电压标定信息时不标伏特。I/Q、分箱 RMS 幅度、弧度相位和复数 IQ 瞬时模峰值包络可选。首次显示、模式切换和自动适配把可见曲线设为绘图区高度的 75% 并垂直居中；用户手动缩放或平移后保留轴范围，直至再次自动适配。Debug/Release UI 测试验证拟合比例、中心偏移和手动范围状态。

宽带与窄带共享指针锚定缩放、范围平移、框选换算和历史辅助逻辑。窄带通道分别持有时间、基带频率、波形幅值及 PSD 功率范围；RF 刻度只改变显示标签，不改内部基带坐标。GPU 图谱数据不经过 CPU 栅格化：线带被扩展为薄三角形并用退化顶点连接，星座/眼图等三角数据也合并为 GPU 三角条带；顶点写入动态缓冲，颜色从单独上传的 LUT 纹理采样。STFT 热图仍由 GPU 对标量纹理调色，坐标、文字和临时覆盖层可保留 QPainter。

资源清理前 Debug 与 Release 的全目标构建和 CTest（4/4）均通过；清理后 Release 全目标重建、CTest（4/4，21.02 秒）通过。Debug 重新链接被本机 `LNK1101: MSPDB140.DLL` 版本不匹配阻断；最小编译/链接探针在已安装工具集中也复现，故这次 Debug 复测未记为通过。宽带和四页窄带硬件烟测均在连接屏幕 2 `Redmi 27 NU`、`\\.\DISPLAY6`、3840×2160、DPR 1.5 上通过，后端为 `QRhi D3D11 | AMD Radeon(TM) 880M Graphics`。宽带每个配置报告 25 次 GPU 数据 draw call；窄带每个配置报告 37 次，四页分别为 22、132、2、130。每页均记录完成帧、顶点/热图资源更新和可见数据绘制；截图可见性抽样 17,280 点均通过阈值检查。

Release 窄带截图：

![Release 窄带信号观察页，3840×2160 / 150% / D3D11](chart-render-release/signal-studio-narrowband-release-gpu-screen2-4k-150.png)

Debug 窄带截图和机器报告见 [`chart-render-debug/`](chart-render-debug/)；Release 窄带见 [`chart-render-release/`](chart-render-release/)。宽带证据见 [`wide-render-debug/`](wide-render-debug/) 与 [`wide-render-release/`](wide-render-release/)。独立 QWidget/QPainter 软件回退记录见 [`chart-software-final/`](chart-software-final/)。

早先专用曲线管线在目标 D3D11 设备上的 draw 回调失败，线带改为共享 QRhi 纹理管线与三角条带后，Debug 和 Release draw 计数及可见窗口截图均通过。smoke 报告现检查截图可见内容，不会再把黑帧仅因尺寸正确记为成功。

仍未完成的验收包括 128 MiB 分析矩阵 LRU 与合并优先队列、本机 3.27 GB IQ 压力记录、SciPy 逐点数值对照、所有模态弹窗的原生操作矩阵，以及星座框选/自适应和完整图表最大化恢复流程。识别和解调仍为合成演示。
