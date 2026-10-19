# Signal Import A1.2 / Brand v2 Execution Report

## 1. Result

执行时间2026-10-10至2026-10-11，工作目录`D:/project/signal_studio_rebuild`。起点`8d0564a`、分支`rebuild/a143-native`，起点工作树干净；用户既有`.gitignore`提交保留。完整阅读任务、当前AGENTS.md和references设计/品牌资源，M0→M5逐节点实现、自查与本地提交。M0..M4提交见CHANGELOG，本文与M5一起提交；完成后普通推送同名远端分支，不改main、不改写历史。

主窗口依据A1.4.3；本次导入依据Signal Import A1.2，品牌依据Signal Studio Brand v2.0。产品版本保留0.1.0，工程格式4、模板格式1。关于页Build是配置时基准提交`3670d34`，本轮M5源代码在其工作树上编译；不能把该字段误认为交付提交SHA，原因见DECISIONS第11项。

必需节点与Windows本地验收全部完成，无P0/P1阻塞。以下明确限制不算作已实现能力。

## 2. Implementation

SampleFormat区分ComplexIQ/Real、Int8/UInt8/Int16/Int32/Float32/Float64；8位无字节序，其余LE/BE。复数IQ/QI/Planar，通道1..128、时刻交织/通道分块、选择通道、uint64头尾、整数可选归一化。启用组合由真实reader、预览、项目与必要分析路径覆盖，没有退回CI16。实数真实单边PSD/STFT、波形与直方图；复数真实PSD/STFT、波形与散点。

RawSampleReader严格完整帧和溢出校验、只读、有界磁盘片段<=32MiB；SourceLoader接近4MiB完整帧块，导航<=2048点。预览<=4MiB实际磁盘数据/16384样本，后台代次取消。停止只保留真实完成块，不把未读尾部当数据；重新读入是完整扫描，错误不添加空节点。模板导出禁止覆盖源及配对SigMF元数据。

原生Widgets还原A1.2页头、快速/批量/模板三页、双栏真实预览与原型加载卡；无WebEngine。单位控件草稿不重排，提交策略与原仓库一致。Fs/Fc来源可见，0Hz合法；未知格式/缺参数必须人工确认。SigMF优先于文件名/模板，显式用户修改优先；冲突不静默覆盖。

批量串行有界调度，逐行编辑、取消、重试、去重和提交隔离；CI16/RI16/CF32混合测试。模板用户128/最近8，导入导出版本1JSON严格校验/原子写；实际导入日志最多1024条。SigMF对照[官方规范](https://sigmf.org/)，只接纳已验证datatype与恒定采集参数，不猜未知编码。

BrandAssets共享资源与缓存；欢迎lockup、标题icon+wordmark、导入32px、窄带24px、关于lockup与构建复制。15个轮廓SVG/PNG回退，PE图标16/24/32/48/64/128/256。原包缺24层，使用原始PNG封装补齐，不重画；运行图标不依赖SVG插件。未改PSD颜色、LUT、游标反色或在数据图上叠Logo。

## 3. Compatibility

工程v4完整记录格式，uint64仍用十进制字符串；v1/v2/v3按旧CI16迁移，完整验证候选后才替换工程，运行缓存不落盘。CI16解码与旧入口逐样本相等；旧功率归一化保留。非归一化整数与浮点轴单位明确，窄带使用源对应比例，不固定32768。DDC改用完整metadata，格式/通道/前缀进入缓存身份；真实CPU FIR/重采样流程保留。

宽窄带时间/频率轴、导航、缩放/拖动/标记、PSD/STFT独立参数、驻留帧PSD、工程入口/最近工程/全局设置均由原用例回归。实数ADC禁止创建复数DDC通道，项目JSON不能绕过限制；Q波形禁用，Fc只是参考，不偏移0..Fs/2谱轴。内置演示资源固定CI16，其它格式覆盖明确拒绝；磁盘RAW可用全部格式。

## 4. Build And Tests

环境：Windows x64、Visual Studio18 2026、C++20、Qt6.11.1 EXACT、CMake4.2或更高。本机Qt路径只在被忽略的CMakeUserPresets中；ShaderTools与Qt同版本，无新增依赖。所有命令退出码均检查，未关闭测试、移除断言或用原型网页测试证明Qt工程。

| 最终验证 | 通过 | 失败 | 证据 |
|---|---|---|---|
| Debug configure/build/deploy/CTest | 7/7，241.62s | 0 | build-debug.log、ctest-debug.log |
| Release configure/build/deploy/CTest | 7/7，41.86s | 0 | build-release.log、ctest-release.log |
| Qt UI Debug/Release | 各87（含init/cleanup），233727ms/38189ms | 各0，跳过0 | ui-debug.txt、ui-release.txt |
| RAW/v4/通道布局矩阵 | 432，旧CI16/迁移/前缀/溢出/非有限值 | 0 | CTest、script-fixtures*.txt |
| 真实DDC格式矩阵 | 72、缓存/直接结果等价、可用前缀 | 0 | 同上 |
| 单音/功率/STFT | 六编码×实复=12个单音，64Hz峰，积分误差<=0.004 | 0 | sample_format_tests.cpp、CTest |
| 独立脚本夹具 | SHA-256与384个参考样本，截断拒绝 | 0 | script-fixtures*.txt |
| SigMF/模板/批量协议 | QtTest6/0，包含混合格式/失败重试/取消 | 0 | CTest内signal_studio_import |
| Debug独立包 | GPU与强制PNG回退各pass | 0 | package-debug-gpu/png.json |
| Release独立包 | 宽带GPU/软件、窄带四页GPU/软件各pass | 0 | package-release-*.json |
| PE/SVG | 两配置7层ICO、15SVG、共享QRC | 0 | brand-debug/release.json |
| 原生捕获/GPU smoke | 10场景pass=true，4K150%与非CPU QRhi | 0 | native-capture-report.json、gpu/ |

可复现命令（仓库根目录，最终执行参数）：

```powershell
.\scripts\build.ps1 -Configuration Debug -Action All -Jobs 8
.\scripts\build.ps1 -Configuration Release -Action All -Jobs 8
# All已调用CTest；单独复查可使用ctest --preset local-debug/local-release --output-on-failure
.\scripts\verify-package.ps1 -Configuration Debug
$env:SS_BRAND_FORCE_PNG='1'
.\scripts\verify-package.ps1 -Configuration Debug
Remove-Item Env:\SS_BRAND_FORCE_PNG
.\scripts\verify-package.ps1 -Configuration Release
.\scripts\verify-package.ps1 -Configuration Release -SoftwareRenderer
.\scripts\verify-package.ps1 -Configuration Release -Narrowband
.\scripts\verify-package.ps1 -Configuration Release -Narrowband -SoftwareRenderer
.\scripts\verify-brand.ps1 -Configuration Debug
.\scripts\verify-brand.ps1 -Configuration Release
.\scripts\gpu_smoke.ps1 -Configuration Release -OutputDirectory docs/acceptance/import-brand-v2/gpu
python tests/fixtures/generate_import_fixtures.py artifacts/import-fixtures-m5
.\out\vs2026-qt611-debug_bin\SignalStudioSampleFormatTests.exe artifacts/import-fixtures-m5
.\out\vs2026-qt611-release_bin\SignalStudioSampleFormatTests.exe artifacts/import-fixtures-m5
.\scripts\measure-import-load.ps1 -Configuration Debug
.\scripts\measure-import-load.ps1 -Configuration Release
$env:QT_QPA_PLATFORM='windows'
$env:QT_SCALE_FACTOR=$null
$env:QT_SCREEN_SCALE_FACTORS=$null
.\out\vs2026-qt611-release_bin\SignalStudioUiCapture.exe --import-brand D:/project/signal_studio_rebuild/docs/acceptance/import-brand-v2
```

脚本目的地须不存在；本轮临时生成目录已经清理，再执行会新建，不覆盖用户源。64MiB零码值CI16实际扫描16,777,216样本：Debug2257ms、峰值33,026,048字节；Release300ms、25,415,680字节。PeakWorkingSet64采样为81/15次，导航1024点；临时文件自动删除。这是独立reader进程测量，不是Qt GUI/GPU峰值或真实射频长期吞吐。

交付前交叉检查摘要/日志/独立包pass/截图10场景及实际文件清单；两份原型样本SHA-256与tests副本相同，程序SHA-256记录在summary.json。源码/文档`git diff --cached --check`通过；原始构建stdout日志保留工具产生的尾随空白，不为满足空白检查而修改原生日志。

## 5. Native UI Evidence

第二连接屏Redmi27NU，原生路径`\\.\DISPLAY6`，3840x2160物理、2560x1440逻辑、DPR1.5（150%）。主窗口实际全屏，非主屏超大窗口替代。QRhi D3D11 / AMD Radeon(TM)880M Graphics，vendor0x1002/device0x150e；宽带独立包22次数据draw、5次顶点上传、1次热图上传，窄带四页31/7/1且每页draw>0。软件路径hardwareRenderer=false、GPU计数0，按软件结果报告。

截图01..10详见验收README。Qt QWidget::grab捕获真实窗口，屏幕截图接口曾返回黑图，未使用该结果；每场景检查非黑像素和屏幕/DPR。主窗3840x2160，导入模态1815x1290、关于810x600均明确为全屏父窗口上的模态裁切，不称整屏截图。03/04/05分别用原包CI16/RI16真实文件各16384样本，06混合两源；07实际128MiB CI8临时RAW停止，保存4,194,304/67,108,864样本前缀；08内置真实IQ经CPU DDC，星座/识别Demo仍标示。

## 6. Files And Executables

实际累计文件清单：`docs/implementation/import-brand-v2/changed-files.json`，由基线到交付索引生成，包含源码、品牌、测试、文档和验收证据，不包含out或忽略的任务目录。核心代码：domain/sample_format、raw_sample_reader、source_loader、import_controller、sigmf_metadata、format_template_store、project_store、signal_import_dialog、source_preview_widget、adaptive_value_edit、brand_assets与source_units。品牌正式目录resources/branding，QRC/RC/ICO自包含。

独立可运行程序：

- `D:/project/signal_studio_rebuild/out/vs2026-qt611-release_bin/SignalStudio.exe`
- `D:/project/signal_studio_rebuild/out/vs2026-qt611-debug_bin/SignalStudio.exe`

从对应目录运行；目录内已部署Qt/qsvg及x64 CRT（Debug含ucrtbased）。运行时不需要外部Qt/source PATH，二进制按仓库约定不提交Git。实现文档在docs/implementation/import-brand-v2，完整证据在docs/acceptance/import-brand-v2。

## 7. Limits And Remaining Work

| 优先级 | 未完成/未验证 | 原因、复现与下一步 |
|---|---|---|
| P2 / 范围允许 | 实数ADC解析复数与DDC | 导入RI16后创建窄带会明确拒绝；需要独立实现并数值验证解析信号/中频变频后再启用 |
| P2 / 显式不支持 | 变频/段头SigMF、未知必需扩展 | 元数据含变化capture频率或segment header时拒绝；需要分段源模型和跨段谱测试，不猜CI16 |
| P2 / 未验证 | 多GB持续吞吐、GUI长期峰值 | 目前实际64MiB/128MiB、有界读取及>2^53模拟索引；下一步用真实长期记录采集内存/取消/吞吐，不外推单次测量 |
| P2 / 未验证 | Ubuntu/OpenGL/Vulkan运行 | 本轮仅Windows原生验收；在对应机器按portable工具链构建并单独记录driver和纹理绘制 |
| P3 / 未验证 | Explorer/taskbar/Alt+Tab视觉缓存 | PE资源与Qt运行图标已验证，未录制Windows shell视觉截图；在目标机刷新系统图标缓存并分别检查三个入口 |
| P3 / 边界声明 | Float64完整DSP精度 | reader随机sampleAt用double，现有分析complex<float>，超float范围明确拒绝；需要升级谱/DDC缓存类型才能承诺64位DSP |
| P3 / 非阻塞 | 编译器警告 | 保留preview局部data遮蔽QWidget::data及旧测试warning；构建成功，下一次小范围清理，不为此改分析架构 |

信号检测、设备输入、真实同步/模型/解调仍是既有未实现范围，未虚构新增能力。没有未声明假实现或未修复测试失败。

## 8. Version Delivery

用户最新指令明确授权各节点commit和最终push，覆盖任务原文默认“不自动提交”的限制。交付只普通推送`origin/rebuild/a143-native`（https://github.com/yulongpo/signal_studio.git），无reset/rebase/force/main更新。本文在最终提交内部，最终SHA及push/远端HEAD/干净工作树的实时结果见最终答复与Git历史，避免在同一提交中伪造自身SHA。
