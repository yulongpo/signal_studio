# UX-R5 独立审核整改

## 首轮判决

独立 Reviewer Hooke（`01a1777a-c3dd-7222-b791-081220262baf`）对 `b5376d1d8e67a5d576b5200486e4cba82fb5302a` 给出 `REJECTED`，发现 R5-REV-01：原始标签虽被服务拒绝修改，仍可从树/图谱/全局菜单进入可编辑重命名对话框，部分删除按钮也未按来源能力禁用。该版本未推送。

原始审核报告和独立复核证据保留于提交外：

- `out/tasks/ux-r5/review/INDEPENDENT_REVIEW-b5376d1-REJECTED.md`
- `out/tasks/ux-r5/review/readonly-menu-ui-observations.txt`
- `out/tasks/ux-r5/review/{debug,release}-directed.txt`
- `out/tasks/ux-r5/review/evidence-audit.json`

## 主代理修复

树、图谱、全局菜单、右侧动作和树快捷按钮统一读取 `AnnotationCapabilities.editMetadata/deleteAnnotation`。批量删除要求全部已选对象具备删除能力，混合来源不能部分执行。`MainWindow::renameMark()` 在打开对话框及取消其他交互前先检查能力；直接命令也不能越过禁用的 QAction 打开编辑器。删除命令同样再次检查完整选择。

只读来源的通道快捷动作明确显示“转换并创建窄带通道…”，继续使用已有显式转换确认；Manual 对应操作保留。没有修改服务只读政策、检测算法、项目事务或持久化格式。

## 整改验证

新增 `uxR5_readOnlyMenusAndCommandGuards`：两种只读来源及 Manual 的全局/Inspector/图谱/真实树 popup 权限、绕过 disabled action 的直接命令、Manual 对话框正向可达、原对象不变与混合选择删除禁用。原有3个 R5 UI 业务函数全部保留，现在该组4个业务函数、Qt totals6（含初始化/清理）。

Debug/Release 完整构建通过；受影响的5组 UX和20个旧 UI业务函数分别重新运行且通过，日志为 `logs/{debug,release}-review-fix-{build,ux-5,ui-20}.txt`。核心服务/存储12组代码未改变，原始通过结果保持，不将旧批次混称菜单修复后的新运行。

原生02场景增加只读全局/右侧按钮和图谱 QAction 能力断言，04场景增加 Manual 能力断言；报告明确区分“检查 QAction”与“菜单在此处实际弹出”。新增 `scripts/verify-ux-r5.ps1 -MenusOnly`，在真实 IQ、普通标签和 fixture worker 数据上，按三种来源实际打开全局、工程树、图谱菜单。工程树和图谱右键由 Win32 SendInput 发出，并核对当前前台确为测试窗口；每个弹出菜单保留截图及实际屏幕身份。该模式只声称3个来源菜单场景，不冒充15场景完整旅程或剪贴板、DDC 验证。

四种配置菜单专项均通过，原目录为 `menus-{release-gpu,debug-gpu,release-software,debug-software}-r3-verified`。每个主场景新增 Win32 `IsWindowVisible/GetClientRect` 实测可见且物理客户区为3840×2160；原生窗口正常显示。完整旅程补测使用另一个新目录 `native-*-verified-r3`，生产独立启动使用修复后 `package-*-r2`，两者各自原始路径写入最终索引，不改名伪装运行批次。首轮76张图仍在此前提交及本机原目录保留。

调度失败另行留档：首次键盘菜单键未打开树菜单；第二次真实右键前台检查失败，随后加入有界前台获取和可见物理窗口校验。`menus-debug-gpu-r3-verified` 的报告和脚本均实际通过，外层调度误读沿用的 `$LASTEXITCODE` 后提前停止，已改为检查真实报告和脚本异常，没有重复运行或改写其通过结果。

完整 Debug GPU 的 `-verified-r2` 曾在第11场景被系统剪贴板 OpenClipboard 错误5阻断，前10场景通过；该次失败不算完整通过。用户恢复后 `clipboard-restored-r2-probe.json` 实测访问成功，随后使用上述全新 `-r3` 目录补测实际完整复制，未以替代缓冲区充当成功。

Reviewer 同时核对了 F2 草稿与 whole-Project swap 的所有权风险：模型持有最多200条自有 Annotation snapshots，草稿提交重新按 ID 查找当前会话，未确认悬垂指针，不新增阻断。该并发组合没有单独动态测试，不声称已实测。

整改由主实现 Agent 完成，Reviewer 没有修改产品。新 HEAD 必须再次经过独立审核，首轮 `REJECTED` 和任何旧版本批准均不能授权发布。
