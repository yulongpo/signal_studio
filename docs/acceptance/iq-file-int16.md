# 交替 int16 IQ 文件读取与图谱验收

## 输入与处理

导入器支持 little-endian `int16` 复采样，字节序列按 `I0,Q0,I1,Q1,...` 解释。文件名读取 `FS`、`FC` 和可选的 `BW` 字段；文件长度必须能被 4 整除。样本计数由文件字节数计算，时间轴保留原始 `uint64` 样本索引。文件以只读内存映射打开，图谱任务仅访问当前视图附近的样本，不把整个文件复制到内存。

用户给定的输入文件属性为：

| 字段 | 值 |
|---|---:|
| 文件大小 | 3,268,608,000 bytes |
| 样本格式 | little-endian `int16 I/Q` 交替 |
| 复采样点数 | 817,152,000 |
| 采样率 | 102.4 MS/s |
| 文件名带宽 | 80 MHz |
| 中心频率 | 830 MHz |
| 时长 | 7.98 s |

时域图可切换 I、Q 归一化分量与 IQ RMS 包络（dBFS）；PSD 使用最多 64 个 Hann 窗做 Welch 平均并以 dBFS/Hz 显示；时频图和瀑布使用复数 FFT。有效带宽默认为文件名中的 BW（缺省时使用完整采样带宽），可在右侧参数区自定义，并将频率视图限制在中心频率两侧。STFT FFT 点数提供 8 至 16 阶选项，默认 11 阶；PSD 和 STFT 选定的 FFT 窗口均完整取自当前可见时间范围，缩放范围不会从视图外借样本，视图会至少容纳两种设置中较大的 FFT 点数。每次时间或频率视图交互都会使分析缓存按新范围重新计算；过期结果不会覆盖新图。色阶提供 Turbo、Viridis、Gray、Plasma、Inferno、Magma、Cividis 和 CoolEdit Classic；工程保存每文件参数，应用设置也会按 IQ 文件路径记住右侧栏参数。CoolEdit Classic 按经典频谱显示中暗蓝低幅度、亮黄高幅度的顺序映射。动态范围与参考电平保留多个下拉候选值，同时支持键盘输入自定义值并立即更新色阶。导航图按全文件时间范围绘制抽取后的 IQ RMS。

## 验证

Debug 构建完成，CTest 为 **3/3 通过**。核心测试以小型 int16 夹具核对文件名参数、样本数、幅度量化、PSD 频率位置和 STFT 输出；UI 测试导入夹具、切换到 PSD、缩放时间视图并等待真实文件曲线与热图完成。

用户指定的 3.27 GB 文件另在 Windows 当前连接的**显示器 2**运行。验收报告为 `real-iq-debug-screen2-4k-150.json`：D3D11 后端识别为 AMD Radeon 880M Graphics，3840×2160 抓图、150% DPR、全屏，真实 PSD/波形、STFT 和纹理上传均完成。带 `--iq-interaction-test` 的运行向主图发送一次实际滚轮缩放事件；报告记录 `wheelEventSent=true`、`timeRangeChanged=true`，缩放后的图谱也完成重新计算。Debug 热缓存运行中，主图 STFT 栅格任务约 542 ms；这只是该机器一次性离线视图计算时间，不代表连续 IQ 吞吐或每帧刷新率。

截图：

- PSD、STFT 与真实滚轮缩放：[real-iq-debug-screen2-4k-150.png](screenshots/real-iq-debug-screen2-4k-150.png)
- IQ RMS 波形、STFT：[real-iq-waveform-debug-screen2-4k-150.png](screenshots/real-iq-waveform-debug-screen2-4k-150.png)

## 参数与界面状态

工程 JSON 按每个 IQ 文件保存波形模式、有效带宽、STFT/PSD 点数、时频显示动态范围与参考电平，以及图谱模式、色阶和坐标范围。窗口尺寸、侧栏/参数分组展开状态、底部标签和最近工程列表保存在用户 `QSettings`；正常启动仍保持空工程，由用户从历史工程菜单打开。

## 边界

工程文件保存 IQ 路径和参数，不嵌入 3.27 GB 样本；重新打开工程时仍需源文件位于原路径。该实现覆盖基础离线读取、I/Q/RMS 三种可选波形显示、PSD、STFT/瀑布和视图交互；I 与 Q 以模式切换显示，不同时叠加。DDC、自动检测、设备输入或长时间连续采集尚未实现。4K GPU 验证针对本机显示器 2，Linux/Ubuntu 尚未实测。
