# U3 验证记录

基线 U2 `f2ec417`，分支 `rebuild/a143-native`。实现、自检及规格/质量
复审已完成；本节点本地提交后才进入 U4，不提前推送。

## 最终自检

| 检查 | 最终结果 | 证据 |
|---|---|---|
| Debug 配置及最终构建 | 通过，最终实现与可执行身份冻结 | [配置](logs/debug-configure-r1.txt)、[构建](logs/build-debug-final.txt)、[身份](diagnostics/input-freeze-r7.json) |
| 完整 Debug CTest | **23/23**，使用原有脚本、preset、runner | [原始日志](logs/ctest-debug-final.txt) |
| 完整 Qt offscreen UI | **108/108**，0 失败、0 跳过，340.778 秒 | [Qt 日志](logs/ui-debug-final.txt)、[JUnit](logs/ui-debug-final.xml)、[逐项计时](logs/ui-debug-final.timing.jsonl) |
| 最终格式/前缀/处理器定向 | **2/2**，7.09 秒，保留原格式断言 | [CTest](logs/core-targeted-r7.txt) |
| Qt 定向旅程 | r6 **8/8**，最终同样由 r7 完整 UI 覆盖 | [日志](logs/ui-targeted-final.txt) |
| 原生 U3 | 最终 r7 **12/12**，真实 IQ 与 **3 次 CPU DDC** | [原生索引](NATIVE_ACCEPTANCE.md)、[报告](native-report.json) |
| 工程持久化 | v6 保存重开、原字节 v5 备份、旧通道不重算 | [新工程](saved-project.json)、[旧字节](legacy-v5-original.json)、[升级工程](legacy-upgraded-v6.json) |
| 静态检查及复审 | 改动 C++ 格式 36/36；四份 PowerShell 语法 0 错；diff check 通过；**SPEC_PASS / QUALITY_PASS** | [格式](diagnostics/changed-format.json)、[语法](diagnostics/script-syntax.json)、[复审](REVIEW.md) |

最终输入及 26 个可执行程序在完整运行前冻结，归档时逐项复核字节数
及 SHA-256 未变。[产物清单](artifact-manifest.json)另记录原生图、
日志及所有引用分片。旧原型、算法 worker 与协议无改动；未将前轮
失败记为通过，完整 UI 的 480 秒保护不变。

## 验证依据与边界

继续使用 `scripts/build.ps1`、项目 CTest runner 和
`scripts/screenshot.ps1 -AnnotationInteractions U3`。完整 UI 的
480 秒终止保护保持原值。offscreen 用例使用 QWidget 回退；
Windows 原生场景另行检查实际 QRhi 驱动和纹理上传。

用户提供的新本机数据位于 `resources/sigmf_data_demo/`，IQ 和 meta
与旧 W: 样本逐字节一致。样本为 20 MS/s、400000 个 CF32 样本；
四条原始标签来自实际 meta，手动和算法标记为明确的 UI 夹具。
通道会对实际 IQ 执行 CPU DDC/FIR/重采样，不能据此证明模型效果。

当前外接屏为 U27U2D / DISPLAY2、连接序号 1、3840×2160、150% DPI。
这是用户明确更换后的实际屏幕；旧 U1/U2 的屏幕证据不作修改。

冻结的 U2 v5 工程引用旧 W: 路径，只验证真实 ProjectStore 的迁移、
原始字节备份及通道快照保留，不重写来源身份或执行旧通道。
新本机 IQ 另行建立 v6 工程，核对创建与重开前后的真实 DDC 输出。

原生空视区场景使用受控可用前缀，和物理文件停止载入分开记录。
处理器回归则通过真实 SourceLoader 停止载入，检查已读前缀尾部、
直接执行及缓存不能读取首个未读样本。

## 首轮问题与处理

- 首次构建发现 `uint32_t` WAV 采样率误用浮点 `QString::arg` 重载，
  已改为整数格式化，保留精确采样率文本。
- 第二次构建发现测试对无 `Q_OBJECT` 的 `SampleTimeEdit` 使用
  `findChild`；修复采用真实 QLineEdit 基类定位，不为测试引入 moc。
- 规格首审发现 LocatedSegment 未统一限制提取频带，以及窄带显示
  仍按 Loading 状态一律拦截。这两项需修复后重新运行和复审，
  不把处理器单独通过当作完整通道流程通过。
- 第三次构建通过；首轮定向四组仅标记领域通过。来源夹具误用
  未注册的算法格式，已改为真实支持的 `detection-v1`；旧“非法版本”
  用例仍写 6，改为未支持的 7，继续覆盖 v6 合法保存。
- Loading 的处理器和窄带旅程暴露 `Int16IqFile` 的底层状态门控，
  已允许非空已读前缀；仍按真实文件完整帧数、指纹和前缀限制读取，
  空前缀、Failed 及越界保留具体原因。不是仅更改界面启用状态。
- 首轮 Qt 定向 6/8，通过与失败均保留；失败分别为上述来源夹具和
  底层 Loading。后续须重跑相同六个旅程，不能仅复用四项成功结果。
- 规格次审发现 MHz 控件显示舍入会误拒绝分数 Hz 的合法默认范围。
  表单现在保留未编辑的精确 Hz，仅实际提交中心或带宽后更新；严格
  包含检查保留，新增默认创建、草稿、Enter/Tab 与快照一致性覆盖。
- 原生 r3 在首个场景前停止：只复制了旧 JSON 索引，缺少原始分片。
  已在 [v5 夹具](fixtures/v5-project/project.json) 恢复原始 JSON 和
  五个配套文件，字节数及 SHA-256 逐项与冻结索引一致；没有修改
  存储校验、旧来源身份或旧数据。最终新工程也归档所引用分片。
- 原生 r4/r5 的前两场景通过，随后鼠标测试未产生 Qt 上下文菜单
  事件而停止。最终采集向实际可见控件投递公开的
  `QContextMenuEvent(Mouse)`，再点击真实菜单动作；不直接调用
  业务槽，也不将此描述为操作系统物理输入测试。
- 质量首审发现名称按 UTF-8 字节限长、保存按 Qt 字符限长，存在
  创建后无法保存的输入。现共用严格 UTF-8、80 个 UTF-16 单位的
  Qt-free 校验，覆盖英文、中文、补充字符、非法编码及编辑重开。
- 旧绑定重载曾只限制提取时间，可能产生保存时被拒绝的越界频带。
  现统一到物理频率后校验完整包含关系，覆盖 RF/Baseband、合法
  裁剪及失败不发布；未改变旧工程迁移中的快照规则。

r6 定向六个 Qt 旅程通过，含初始化/清理为 8/8、零跳过、
48.898 秒；相同用例随后也由最终 r7 完整 UI 覆盖。

r6 首次完整 Debug 回归为 22/23：Qt UI 108/108、0 跳过，
407.009 秒（CTest 407.37 秒），低于原 480 秒保护；非 UI 21/22。
`sample_format` 发现内置资源的固定格式诊断被新增的空前缀检查
遮蔽。保留原断言，提前验证内置资源格式；空前缀、Failed 和
Loading 前缀边界仍保留。该次失败不计入最终通过。

格式自检另修正五处改动上下文的缩进及换行，最终 36/36 文件通过；
只读质量复审确认没有表达式、作用域或控制流变化。最终 r7 使用
重新构建的版本并重新完成全部 23 项，先前结果保留为历史记录。

原始中间日志保留在 `out/tasks/wideband-detection/U3/`，不计入通过。
