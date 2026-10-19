# M0-P 浏览器验证

日期：2026-10-13。原型目录：`docs/prototype/wideband-detection-r1/`。检查脚本：`scripts/verify_wideband_prototype.mjs`；逐项结果见 [browser-checks.json](browser-checks.json)。这份记录只验证离线浏览器交互原型。

## 方法与重现

使用系统 Microsoft Edge 的 headless 浏览器，viewport 为 2560×1440，deviceScaleFactor=1；真实鼠标、键盘、菜单、文件上传、下载与 localStorage 操作结合只读 snapshot 断言。任务阶段由用户可见按钮显式推进，没有模拟定时进度。浏览器版本与记录时间存入结果 JSON。

本机命令（项目根目录）：

```powershell
$env:SS_PLAYWRIGHT_MODULE='C:\Users\Admin\.cache\codex-runtimes\codex-primary-runtime\dependencies\node\node_modules\playwright'
node scripts\verify_wideband_prototype.mjs
```

脚本默认通过 file URL 打开原型，也支持 `SS_PROTOTYPE_URL` 指向本地 HTTP 页面。本次 Playwright 版本为1.62.1。Playwright 是验证工具；原型运行不需要 Node、Playwright、服务器或第三方库。系统 Edge 通过 `channel: 'msedge'` 启动。

最终候选回归：40 组检查，0 个失败，0 个 JavaScript 错误。逐项结果以 JSON 实际记录为准。首轮规格及质量发现的问题、修复与复验见 REVIEW.md；没有将失败结果算作通过。未修改原生源码，因此本阶段没有运行 Qt 构建、CTest 或 GPU smoke。

## I01–I24 实际覆盖

| ID | 本轮已操作/断言 |
|---|---|
| I01 | 三入口进入同一配置卡，run 数不增加 |
| I02 | 运行快照不随缩放改变；短 ROI 请求与必要上下文分开，上下文不越可用前缀 |
| I03/I04 | 持续标记进入 ROI，Esc 回原工具；Enter 单独提交；拖出边缘时裁剪合法 ROI，无标记/任务副作用 |
| I05 | 背景框选缩放、轴单独平移、滚轮到边界；保存标记几何不变。视图浮点相等采用小于样本/频率精度的数值容差 |
| I06 | 同一选择切时频/瀑布、RF/基带，物理记录及 ID 不变 |
| I07 | 重叠来源选择列表、Ctrl/Shift 多选，表与树选择一致 |
| I08 | 创建手动对象、八手柄分别提交一次修订、undo/redo；撤销旧复核不删除随后发布的算法结果 |
| I09/I10 | 标签先只读再修订，origin/source/ID不变；复核绑定revision，边界修订回Pending；重复接受不复制对象/决定 |
| I11/I12 | 来源/mean筛选、数值分数排序、只选可见与批量复核；未选对象不变；图层显隐不改存储 |
| I13 | sample/ROI/coarse互斥参数，关闭拆分禁用对应参数；SCN16帧、FFSCN-17频率阶数/10帧；三方案分别恢复提交值 |
| I14/I15 | 输入中不应用；refresh和blur事件保留草稿；Enter/Tab/ShiftTab/应用内点击提交；输入/下拉滚轮不改参数，侧栏滚动保留 |
| I16 | 发布后安全取消保留对象，取消中/取消完成分开；S0–S9及失败/零结果/缺模型/短上下文场景有别 |
| I17 | A任务运行、浏览B，从后台页按runId推进A；B标记不变、输出归A；新工程清理任务与草稿 |
| I18 | 时间未知只有observation，没有活动段且不可建通道；coarse/sample/ROI/fallback/manual证据可查 |
| I19 | 同对象拆分、合并、添加、删除段，origin/source不变；没有跨来源融合 |
| I20 | 两类内置与本地真实夹具预览、重复导入幂等；生成器12载波/17TIME段，FC-Fs/2及10µs量化；TorchSig换序JSON接受、类别冲突拒绝 |
| I21 | 冲突禁止导入；Partial标签尾部仍按完整物理样本保留 |
| I22 | 输出率错误拒绝；有效段确认后建通道；来源改动后通道范围和revision保持旧快照，显示changed |
| I23 | 保存、重开、demoOnly下载；模拟保存失败保持对象；缺layout、坏params、坏样本范围上传时拒绝并保持工程/文件/任务/布局 |
| I24 | 三分隔条拖动、三面板折叠恢复、主图最大化/恢复；图谱有空间、文档无横向溢出、标记几何不变 |

独立规格审查额外验证：完整20ms前缀仍处于Loading时拒绝启动；真实生成器修订/原文/量化语义；通道旧revision；损坏导入及配套长度/类别冲突。最终评审结论另见 [REVIEW.md](REVIEW.md)。

## 截图与边界

截图保存在 [screenshots](screenshots/)，包含 Ready、ROI待确认、运行快照、安全取消、复核证据、导入冲突、通道来源变化、跨文件后台任务和调整后的布局。版本/资产指纹由 `artifact-manifest.json` 冻结。

上述 2560×1440 是浏览器 viewport，不是第二连接屏4K/150%原生验收。blur 检查派发浏览器事件，不证明 Windows 前后台切换或原生下拉弹出列表行为；八手柄逐一验证在时频模式，未穷举所有跨模式组合和全部可能标签变体。真实 Qt、100000段规模、真实IQ/WAV reader、模型前向/效果、worker、DDC、部署和GPU显示属于M1–M5。

标签字段/几何来源是已冻结真实元数据；波形、热图、算法分数、噪底、任务计数、通道均为合成演示。导出的状态含demoOnly，不能作为原生工程或真实算法结果导入。
