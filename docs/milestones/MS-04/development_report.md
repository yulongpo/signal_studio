# MS-04 开发报告

MS-04 把既有十模块平台组装为可运行的 Signal Studio 基础产品。应用核心通过公共
C++20 契约编排工程、外部数据源、TaskRuntime、DSP/Compute、Visualization、
Workbench、Inspector 和 Artifact，不把 Qt 或第三方类型引入公共平台接口。

最终应用提供工程新建/打开/保存/关闭、RAW/IQ/WAV 显式确认与有界导入、暂停/继续/
取消、基础 PSD/STFT、P01/P02/P03/P05 原生页面、来源可追溯结果提交与无覆盖导出。
五个 Qt Designer `.ui` 文件由生产目标实际编译使用，W01/W05 及多尺寸、多 DPI
证据由最终可执行程序直接生成。

VS Code 默认 F5 直接构建并调试 CPU Debug `SignalStudio.exe`；安装树同时提供最终
应用、十组件 CMake 包、Qt/VC143/oneMKL/libsamplerate 运行时和 Qt 创建前自检。

完整实现细节见 [MS-04_实施记录.md](MS-04_实施记录.md)，界面对齐结论见
[MS-04_UI差异报告.md](MS-04_UI差异报告.md)，验证数据见
[MS-04_测试报告.md](MS-04_测试报告.md)。

最终 8 小时稳态、Windows CI 和验收记录均已通过，MS-04 已关闭。当前按用户指令
停止，获得确认前不进入 MS-05。
