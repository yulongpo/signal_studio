# Import A1.2 / Brand v2 Milestones

| Node | Status | Gate / Evidence |
|---|---|---|
| M0 | Complete | BASELINE_AUDIT.md；干净工作树、fetch、基线审计 |
| M1 | Complete | Debug build；CTest 6/6 (243.80s)；144组格式/读取/v4往返；旧CI16、DDC、谱分析、UI回归 |
| M2 | Complete | M2-ui.txt 6/6；第二屏原生捕获8场景，CI16/RI16实际文件、真实停止前缀；批量/模板深化在M4 |
| M3 | Complete | M3-ui.txt 3/3；共享15SVG+PNG回退；PE七层ICO；第二屏10场景截图；Debug独立包 |
| M4 | Complete | M4-protocol.txt 6/6、M4-ui.txt 5/5；混合队列/SigMF/模板原子往返/端到端导入；原生截图 |
| M5 | Complete | 最终Debug/Release CTest各7/7、UI各87/0；432格式+72DDC；独立包GPU/软件、第二屏10张原生截图、完整执行报告 |

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

## M4 自查

SigMF对照官方1.2.6，6种已验证编码映射、0Hz、采集来源冲突、人工覆盖、同目录路径校验、未知/不支持字段反馈。未知datatype、跨目录、分段变频、必需扩展、坏JSON/预算超限均拒绝，不降级CI16。文件模板保留Fs/Fc/BW，已有明确格式被覆盖前确认，缺参数仍需人工确认。

批量串行读取，各源成功/失败/取消/partial独立，逐行编辑、移除、重试；同路径+解释参数去重，修改后再次检查。已成功提交标记避免回调重试重复加入工程。用户模板JSON版本1/QSettings、最近8项、原子导出/严格导入，输入样本禁止作为导出目的地。原型模板页三列卡、导出按钮、保存底栏已完成，不再是单行占位。

协议测试6 passed / 0 failed；UI端到端5 passed / 0 failed，真实SigMF冲突覆盖、混合CI16/RI16、模板往返/拒绝源写入、原生工程加入、快速切换关闭。首次UI模板写失败源于缺组织名，修正组件设置并使用临时注入配置后通过。重新捕获10场景pass=true，模板卡继承边框问题已视觉检查修正；M5再次生成最终截图。

## M5 自查

最终Debug配置、构建、部署、CTest全部通过：7/7，241.62s，Qt UI 87 passed / 0 failed / 0 skipped；Release同链通过：7/7，41.86s，UI 87/0/0。没有关闭测试或删除失败断言。432格式/读取/项目矩阵、72真实DDC格式组合、12实/复单音功率与各编码STFT、独立脚本SHA与384参考样本全部通过。批量CI16/RI16/CF32、单条失败、取消、重试、重复和提交隔离通过；大索引以uint64与模拟源验证，不创建多GB测试输入。

自查发现窄带读取仍有旧path入口和固定32768比例，已改为完整FileMetadata和源格式比例；补测全编码DDC及缓存等价。实数ADC单边轴/游标、Q禁用、浮点与未归一化整数功率单位、坏项目不能绕过实数DDC限制均修复并回归。内置演示资源固定CI16，不允许用其它格式静默重解释。预览坐标文字和批量删除按钮经原型/原生图对照修正，不改信息架构或加载卡布局。

Debug独立包正常/强制PNG、Release宽带GPU/软件与窄带四页GPU/软件全部pass；输出目录运行且PATH仅系统目录。PE七层ICO与15个SVG检查两配置通过。第二连接屏Redmi 27 NU / DISPLAY6 / 3840x2160 / DPR1.5上，QRhi D3D11 / AMD Radeon 880M实际纹理上传及数据draw通过。10张真实Qt截图pass=true，主窗口全屏；模态裁切与实际输入单独记录。实测128MiB停止夹具保留4,194,304完整样本前缀，临时文件已删除。

64MiB实际只读扫描Debug2257ms/33,026,048字节峰值工作集，Release300ms/25,415,680字节；这只是reader进程单次测量，不声称通用吞吐或GUI峰值。原始两样本未修改，脚本临时夹具已清理。交付文档、最终日志、截图、机器摘要与实际变更清单齐全。限制逐项见CODEX_EXECUTION_REPORT.md：实数解析DDC、变频SigMF、Ubuntu/Vulkan、多GB持续吞吐及Windows shell图标视觉缓存没有伪装为完成。M5通过后本地提交，再普通推送当前分支；最终SHA/远端一致性以Git和最终回复核验。
