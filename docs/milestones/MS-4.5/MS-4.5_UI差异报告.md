# MS-4.5 UI 差异报告

## 1. 交付界面

P03 Inspector 增加由 `SignalAnalysisSettingsPanel.ui` 经 `qt_wrap_ui` 编译的原生参数
面板。面板通过 Workbench Inspector extension 安装，Qt 类型仍不进入 Workbench
公共头。生产界面没有使用静态标签冒充计算结果；点击应用后，参数快照进入后台 DSP，
完成的同代际不可变帧才由 GUI 线程绑定。

## 2. 基础与高级模式

基础模式显示预设、FFT、窗、估计器、平均/保持、平滑、STFT 重叠和显示映射。高级
模式增加帧长、补零、频谱与 STFT 各自的 Kaiser/Tukey 参数、Welch 段数、
Savitzky-Golay 阶数、单/双边、归一化、STFT 独立帧/FFT/hop/边界及自定义滤波系数。

派生区实时显示频点数、bin spacing、RBW、STFT 时间步长/行列数、主机/设备内存、
FFT 次数、估算运算量、性能等级和参数错误。无效参数在提交任务前阻止，并保留上一
有效结果。

## 3. 操作与状态

- 应用全部、仅应用 PSD、仅应用 STFT、共享关键参数；
- 取消当前后台分析，过时 `ViewRequestId` 不提交；
- 保存/删除用户预设，恢复当前视图默认或软件默认；
- 参考电平、动态范围、Industrial/Viridis/Turbo/Inferno/Grayscale 色表、插值和频率轴模式
  即时真实更新图谱像素/坐标，不重算 DSP；
- 状态栏与结果来源显示参数哈希、实际 oneMKL/cuFFT 后端和设备。

## 4. 与批准页面的边界

批准 P03 的容器、Inspector 和来源语义保持不变。本里程碑只扩展频谱/时频参数编辑，
不增加 MS-05 的 Selection 建通道、DDC、重采样输出、多通道继承和宽窄带联动，也不
增加 MS-06 的模型、识别、解调或插件结果。

谱线平滑和时频平滑不是对批准 198 项原需求的静默改写，已在开发需求追踪矩阵交付
副本中分别登记为 `ENH-SPEC-001` 和 `ENH-SPEC-002`。

## 5. 尺寸、DPI 和证据

自动化覆盖 1280×720、1920×1080、3840×2160 及 125%、150%、175%、200% DPI，
检查面板加载、控件命中、布局边界和逻辑/物理尺寸。最终 qwindows 证据为：

`evidence/MS-4.5_P03_高级参数面板_1920x1080.png`

该截图使用 1920×1080 逻辑窗口、Windows 150% DPR，物理 PNG 为 2880×1620，显示
真实 X310 数据、PSD/STFT、参数派生信息、参数哈希和 oneMKL-DFTI 来源。
