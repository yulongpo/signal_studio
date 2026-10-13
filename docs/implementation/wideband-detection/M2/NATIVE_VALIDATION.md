# M2 原生证据与原型对照

冻结依据为 M0-P `wideband-detection-r1.0`，提交 `c638cb92d0ed56a127d49618934b45638d7ecd73`，及规划07。原型22文件指纹全部一致。M2 退出前按专项 → `scripts/build.ps1`完整回归 → `scripts/screenshot.ps1 -WidebandDetection` → 原型截图对照 → 本地提交执行；最终Debug CTest 15/15、Qt UI 94/94、第二屏23/23原生场景通过。

完整记录：[原生报告](native-report.json)、[23场景截图](screenshots/)、[构建脚本CTest](debug-ctest.txt)、[Qt UI逐用例记录](debug-ui.txt)、[用例时长](debug-ui-timing.jsonl)、[原生脚本结果](native-capture.txt)、[图像指纹](artifact-manifest.json)。主Agent已查看全部场景，最后ROI/三列修补的对应图再次对照冻结截图。

实际输入为用户指定 `W:/wbSig53_rfbench20m/train/baseline/0000000.sigmf-data` 及配套meta/JSON：CF32、20 MS/s、Fc 2.5 GHz、400000完整复样本、20 ms、4个标签，IQ SHA-256 `3a47e55b5f02851d702d5c5782998bf3e374f67cde592c35689acf9ec5399254`。测试通过真实控件增加1个手动对象并完成系统链路夹具任务，发布3个没有精确时间段的算法观察对象；不伪造模型或精修结果。

原生主窗口必须使用第二连接屏Redmi 27 NU、Windows设备路径DISPLAY6、3840×2160物理、150%系统DPI、2560×1440逻辑全屏。卡片与弹窗图是该真实窗口/屏上的可见控件裁剪，不是软件离屏图片。报告逐图记录屏幕身份、尺寸和实际QRhi帧。

| 契约与冻结截图 | 对应原生场景 | 核查重点 |
|---|---|---|
| P01–P03 / ready | 01、02及02可见卡片裁剪 | 当前文件下方同一卡片；四范围、物理时长、策略矩阵、固定操作区 |
| P04 / roi-confirm | 07–10 | amber ROI和顶部居中的物理范围；Esc恢复持续手动工具，Enter只提交ROI；对象/任务/视图不变 |
| P08/P10 / running-frozen | 03、04 | 可读任务卡、来源文件、不可变请求、覆盖/未读/排除、窗口数；技术JSON单独折叠 |
| P09–P12 / ready、review-evidence | 05、06 | 4标签+1手动+3观察；来源图层常驻；mean首屏，max在证据；修订回Pending、保存重开 |
| P05/P17 / review-evidence | 17–19 | 时间/精修、分数/唯一窗口、噪底/回退三列；未保存数据明确暂无；原始字段通过真实折叠及滚动可见 |
| P14 / channel-source | 20–22 | 只确认活动段，整对象仍Pending；通道checkbox实操校验；取消不建通道；保存重开不丢确认与来源 |
| I24 / adjusted-layout | 11–16 | 三手柄实际拖动/释放、Esc回退；8对象几何/revision/source/run不变；固定运行取消按钮完整可见 |

初始侧栏278/390、底部展开260使用冻结原型默认；按规划07保留A1.4.3三图比例和既有手势。原型的合成波形/热图不作为真实IQ图像期望；原生初始可见10ms和完整20ms文件范围分别明确显示。新增31px图层栏与5px拖动条占用计入主图高度检查，不扩大窗口或压缩字体替代。

M2只验收数据与离线任务链、已确认的交互骨架。真实模型前向、sample/ROI精修、二维效果、真实算法段→DDC、全量S0–S9/I01–I24的最终A16与Debug/Release部署继续按M3–M5执行。捕获报告的`modelInferenceVerified`和`a16AcceptanceVerified`保持false；本文件的原型对照不把它们改成true。
