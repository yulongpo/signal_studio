# G0 现状与风险核对

2026-10-23 在 `D:/project/signal_studio_rebuild` 核对本地 main 与 origin，执行 fetch 后：本地 `36a509d56764457524faa72d9af6ff95429bcb1c`，目标远端 `90be58e452f17b121028efc8b34af9be50a76e6e`。本地含额外 main 合并历史，目标为祖先，文件树一致。工作区原有未跟踪内容按初始化清单保留；新功能使用 `feature/ux-r3-optimization`。

完整阅读本轮主文件、项目 AGENTS、README，以及相关实现和既有证据边界。已确认已有真实样本读入、PSD/STFT、CPU DDC、TensorRT worker、标记修订与五类检测状态/覆盖记录；增量实现复用这些路径。旧项目为 v6，入口原为目录/project.json；轴 left 原为 88 逻辑像素；星座和眼图原同页，标签原依赖手动指定可信 profile。

G0 原生屏幕探针确认：连接 2、Redmi 27 NU、`\\.\DISPLAY6`，首选/实际 3840×2160，150% DPR，逻辑全屏 2560×1440。第一屏 TL160ADMP36 为 2560×1600，不参与要求的实屏验收。基线调用现有 Release 部署的 `scripts/screenshot.ps1`，得到 pass 的原生截图和 QRhi D3D11/纹理证据；这是修改前已有二进制的现状记录，不冒充本轮新编译结果。

原始输出：`out/tasks/wideband-detection/UX-R3/G0/diagnostics/baseline/`，日志位于相邻 `logs/baseline-native.txt`。随后 Debug 初步编译及工程树定向测试 1/1 通过，日志 `initial-integration-build.txt`。工程格式和标签首轮失败另列在 VALIDATION，不覆盖或改写历史记录。

主要风险由行为测试解决：外部资产与源引用移动、另存和取消不改原工程、未知 TXT 不猜单位/编码、旧页面值映射、后台代次、轴几何与坐标命中、实际部署目录隔离。最终必须由未参与实现的 Review 子代理复核最终 HEAD，批准前禁止任何远端同步。
