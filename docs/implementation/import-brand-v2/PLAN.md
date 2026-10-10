# Import A1.2 / Brand v2 Milestones

| Node | Status | Gate / Evidence |
|---|---|---|
| M0 | Complete | BASELINE_AUDIT.md；干净工作树、fetch、基线审计 |
| M1 | Complete | Debug build；CTest 6/6 (243.80s)；144组格式/读取/v4往返；旧CI16、DDC、谱分析、UI回归 |
| M2 | Complete | M2-ui.txt 6/6；第二屏原生捕获8场景，CI16/RI16实际文件、真实停止前缀；批量/模板深化在M4 |
| M3 | Complete | M3-ui.txt 3/3；共享15SVG+PNG回退；PE七层ICO；第二屏10场景截图；Debug独立包 |
| M4 | Pending | SigMF/来源/模板/队列隔离/取消/重试/去重测试 |
| M5 | Pending | Debug/Release/CTest、独立包、GPU/软件、九张截图、执行报告 |

每节点 Gate 通过后更新自查与证据并本地提交。所有节点完成后正常推送当前分支，不强制同步。UI 按 A1.2 信息架构和视觉尺寸还原，主窗口仍遵守 A1.4.3。未实现项不能用模拟结果替代。

## M1 自查

完整覆盖六编码、实/复、字节序、三种复数布局、通道交织/分块、选中通道、头尾及完整帧。格式 JSON 严格验证；v4 字段往返，v1/2/3 回填旧 CI16。旧 CI16 归一化数值逐样本相等，真实实数 PSD 单边功率积分匹配 0.125。读取块按完整帧、32MiB 片段预算、导航<=2048；非4字节帧停止/前缀重读通过。缓存包含格式/代次/前缀；实数 DDC 在业务与处理器层拒绝。

命令：`scripts/build.ps1 -Configuration Debug -Action Configure`、`-Action Build -Jobs 8`、`ctest --preset local-debug --output-on-failure`。原生日志见 M1-ctest.txt / M1-ui.txt。M2 开始前已完成旧功能回归。

## M2 自查

快速导入、批量、模板三页按 A1.2 结构还原：73px 页头、48px 页签、83px 文件条、弹性两栏、PSD/波形+散点或直方图/STFT、66px 底栏。表单与预览分别滚动，格式摘要徽标与三项统计对照原图；高级参数互斥。加载遮罩使用525px居中卡、三个23px圆形阶段、9px实际进度条和原型停止/保留/重新完整读入文案。

原型模拟数据不参与生产预览，CI16/RI16均以实际16384样本计算。未知格式须人工确认。自适应输入在草稿期间不重排，Enter/Tab/外部点击提交，窗口失焦不提交；高级整数同策略。串行导入在完整扫描后提交工程，无有效样本不创建节点，停止保存完整前缀。

`SignalStudioUiTests importFormatAndAdaptiveDraft importRealFilesAndCancelPrefix addIqFileDialogCancelsAndImportsRealInt16Iq parameterInputsIgnoreWheel`：6 passed / 0 failed。首次调用有一个不存在的测试函数名，修正命令后重跑通过，没有删除断言。

`SignalStudioUiCapture --import-brand <acceptance-directory>`：8场景通过，第二连接屏 Redmi 27 NU / DISPLAY6 / 3840x2160 / DPR1.5。主窗口全屏；导入图是该屏真实Qt模态窗口裁切，报告记录实际像素/DPR。系统桌面抓图返回黑图，改用既有 QWidget::grab 并加非黑像素检查。具体证据为 docs/acceptance/import-brand-v2/native-capture-report.json 与 screenshots/03..07。M3/M5将重新生成全品牌截图。

## M3 自查

正式资源完整复制，qrc路径有效，共享资源静态库只编译一次，三个可执行目标都锚定可用。原包ICO缺24px，使用原始PNG尺寸封装七层，PE资源实读16/24/32/48/64/128/256。15个SVG无脚本、图像、文字依赖；所有变体和PNG回退资源测试3 passed / 0 failed。

欢迎、标题、导入32px、窄带24px、关于与运行窗口图标已集成；图谱颜色/纹理策略未改变。原生捕获10场景pass=true，包含08窄带真实DDC和09关于，第二屏实际4K/DPR1.5；模态尺寸按报告记录。截图切换工程的确认最初未通过，修正为点击真实Yes按钮后重新捕获通过。独立包Debug系统PATH通过，强制PNG回退同入口再验证，完整证据见BRAND_USAGE.md、brand-debug.json与M3-package-png.txt。Release验证在M5执行。
