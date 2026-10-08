# Signal Studio · A1.4.3 原生基础骨架

本工程从空仓库独立开发，以仓库内的 [A1.4.3 原型基线](docs/prototype/a1.4.3/BASELINE.md) 为依据。旧 Signal Studio 与 ISA 不参与源码、构建或运行。当前分支为 `rebuild/a143-native`。

采用 C++20、Qt Widgets 和 QRhi。Windows 默认使用 Direct3D 11；同一渲染接口可选择 OpenGL，为 Ubuntu 原生支持保留路径。本轮实际构建环境为 Windows x64 / VS 2026 / Qt 6.11.1。QRhi 的底层接口依赖 `Qt6::GuiPrivate`，因此锁定 Qt 6.11.1；升级 Qt 时需要重新编译和验证。

## 当前能力

- 工程树、全局时间导航、可切换波形/PSD 的辅助图、时频图/瀑布图主图、右侧参数区和默认收起的底部日志。
- 每文件保存视图、图谱模式、颜色、显示参数、标记、多选及演示通道；允许空工程。
- 主图右键开启持续选择；普通点击选择，框内移动，边线及控制点调整，Ctrl/Shift 多选、批量删除、定位与重命名。
- 空白主图区框选缩放，物理轴滚轮缩放/拖动平移，导航定位，每文件最多 40 条视图历史；主图滚轮 220 ms 合并。Esc 取消拖动并回滚。
- 原生工程 JSON 的完整校验、原子写入和状态往返。样本索引采用 `uint64_t`，JSON 用十进制字符串，频率采用 Hz。
- QRhi 持久热力图纹理、独立覆盖层纹理与 GPU 合成。颜色变化复用模拟功率矩阵；标记变化复用热力图纹理。启动显示实际后端，失败时回退到软件绘制。

**当前所有图谱和 IQ 文件都是演示数据。** PSD/STFT 参数保存于文件状态，STFT FFT 控制缩放下限；尚未读取真实 IQ，也未执行 FFT/STFT、DDC、检测或设备采集。演示通道只保存创建时的参数，修改源标记不会重新计算通道。

## 构建与运行

本机已生成被 Git 忽略的 `CMakeUserPresets.json`，记录 Qt 路径。其他机器可设置 `SS_QT_ROOT` 为安装的 Qt 6.11.1 kit，然后使用公共 presets。

```powershell
.\scripts\build.ps1 -Configuration Debug
.\scripts\build.ps1 -Configuration Release
.\scripts\run.ps1 -Configuration Release
```

构建脚本默认依次执行配置、编译和 CTest。输出目录：

```text
out/vs2026-qt611-debug_bin/SignalStudio.exe
out/vs2026-qt611-release_bin/SignalStudio.exe
```

Windows 自动使用 `windeployqt` 部署运行库，`qt.conf` 指向本目录插件。可直接从对应目录启动。独立运行验证会清理 Qt 环境并将 PATH 限制为系统路径：

```powershell
.\scripts\verify-package.ps1 -Configuration Release
.\scripts\verify-package.ps1 -Configuration Release -SoftwareRenderer
.\scripts\screenshot.ps1 -Configuration Release
```

命令行诊断：

```powershell
.\out\vs2026-qt611-release_bin\SignalStudio.exe --smoke-test --require-gpu --render-report out/render-report.json
.\out\vs2026-qt611-release_bin\SignalStudio.exe --renderer opengl
.\out\vs2026-qt611-release_bin\SignalStudio.exe --software-renderer
```

`--require-gpu` 必须识别为硬件设备、成功上传热力图纹理，并通过覆盖层更新时复用纹理的检查。无界面的 Qt UI 测试走软件路径，用于验证事件和业务状态，不能作为 GPU 证据。

## 模块边界

| 目录 | 职责 |
|---|---|
| `domain/` | 元数据、样本/频率范围、标记、通道、显示状态；无 Qt 依赖 |
| `application/` | 每文件会话、选择、业务命令和视图历史；无控件依赖 |
| `infrastructure/` | 原生 JSON 读写和校验 |
| `app/` | 程序入口、菜单、工程树、参数面板与工作区编排 |
| `ui/charts/` | 坐标映射、手势、演示绘图、QRhi 渲染及着色器 |
| `tests/` | 状态/存储与 Qt 事件回归 |
| `docs/` | 原型冻结副本、架构、验收与实际验证记录 |

渲染器只接收图像、目标矩形和覆盖层，不读取工程存储或执行信号算法。真实 IQ 接入时应新增独立数据源与后台计算服务，通过不可变结果提交到 UI；当前还没有这些计算服务。

## 工程格式与验收边界

原生格式为 `signal-studio-native-project`、版本 1，与浏览器的 `signal-studio-a1.4.3-prototype` 分开。不能直接打开归档的原型演示 JSON；它是产品基线样例。原生工程不包含样本数据或视图历史，也没有业务撤销栈、自动保存及未保存提示。

完整 A1.4.3 的 59 项原型用例仍需逐项移植。分隔条取消/双击复位、属性分组自动前置、全部刻度碰撞处理、辅助 Y 范围进入视图历史、真实大文件、持续数据流的纹理行更新、GPU 曲线顶点缓冲和 Ubuntu 部署验收留在后续阶段。当前覆盖层先由 QPainter 生成缓存图像，再由 QRhi 合成，不声称曲线和文字已全部改为 GPU 几何绘制。

实际通过的检查见 [验证记录](docs/acceptance/verification.md)，产品行为清单见 [骨架验收清单](docs/acceptance/skeleton-checklist.md)，渲染选型见 [渲染架构](docs/architecture/rendering.md)。
