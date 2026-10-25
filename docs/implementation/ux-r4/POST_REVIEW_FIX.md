# 独立审核后修复与当前状态

首次独立审核对象 `26b9fffd378f1cbd54401f59dc74d4337535ffd1` 的结论为 **REJECTED**。
原始独立报告完整保留于 [review-history/26b9fff-REJECTED.md](review-history/26b9fff-REJECTED.md)。
此报告不能批准后续版本；没有向远端推送任何 UX-R4 提交。

## 已修复

- R4-IR-01：共享证据现在包含完整主来源 `originalText`、完整补充来源
  `supplementalOriginalText`、主/补充 hash，以及来源集合的完整非原文元数据和标记引用。
  `encodeAnnotation` 中引用的大型记录所指向的补充全文不再因展示层遗漏而丢失。
- 只读原文视口按 65536 UTF-16 字符单元分页；跨页查找与上一/下一页可访问全部内容。
  剪贴板使用独立完整缓冲，分页不截断复制，UTF-16 代理对不拆开显示。
- 搜索 Enter 不再误触对话框自动默认翻页按钮；新增行为测试已复现并验证修正。
- 辅助证据页隐藏时清空其上下文并关闭非模态详情，防止切换文件/对象后遗留旧窗口。
- 系统剪贴板暂不可用时进行有限重试；失败显示可重试状态，切换上下文取消旧请求。

## 用户修订与实际验证

用户最新明确要求：**“不用执行大型RFbench事件验证”**。本次修复后没有执行大型 RFbench
事件验证，也没有重新运行包含大型事件用例的 label_import 全套。先前首次审核前运行的合同日志
是历史事实，不冒充此次重新执行。

新增测试以常规大小真实 TorchSig SigMF/RFbench 文件进入现有可信解析器，分别检查 Inspector / Auxiliary
两处原文可见、主/补充全文可复制、补充 header 未知字段保留、hash 与引用存在、来源未被改写。
分页另用约 140k 字符的合成原文字段验证，非大型 RFbench 事件输入。

Debug/Release 均构建应用、原生 runner 和两个 Qt 测试程序。修复后每配置补跑：

- UX annotation/layout/charts/popovers：4/4，通过；annotation 含 4 个业务用例。
- UI annotations 定向 11 个业务用例：1/1，通过。
- 共 5/5 受影响 CTest 组，每配置 29 个 UI 业务函数；没有重跑无关算法矩阵。

修复版还从 Debug/Release 部署目录以 system-only PATH 分别执行 GPU / 软件独立启动，4/4 通过；
这些结果证明独立启动及 renderer；下述恢复后的原生流程另行证明复制与完整交互。

原始构建、首轮失败、修复后的 Qt 日志和原生失败报告归档于 [evidence/review-fix](evidence/review-fix/)。

## 原生复制的阻塞及恢复

新版本 Release GPU 原生运行已完成前六个场景，随后复制操作无法读回系统剪贴板。
Qt 原生日志包含 `OleSetClipboard` / `OpenClipboard` 失败，COM 错误 `0x800401d0`；
独立只读 Win32 探针也确认 `OpenClipboard` 返回 false，错误 **5 / Access Denied**，
运行环境为 `WinSta0`、输入桌面 `Default`。探针不读写剪贴板内容。
已清理主代理和独立审核代理各自闲置的 Computer Use 会话，系统状态仍未恢复。

这是当时主机的实际阻塞，不能将其误报为原文截断或复制通过。详见
[clipboard-status.json](evidence/review-fix/clipboard-status.json)。随后用户恢复系统剪贴板；
未擅自关闭用户应用或修改系统安全/隐私设置。

恢复后 [系统探针](evidence/review-fix/clipboard-after-restore.json) 返回 OpenClipboard=true、错误 0。
修复版四组原生程序全部通过，每组 21 主场景及详情，复制 38987 字符。
右侧与辅助两处使用常规真实 TorchSig 来源，补充全文/hash 实际读回匹配，原文搜索也通过。
四组报告均记录 ordinarySupplementalEntriesVerified=2、largeRfbenchValidationExecuted=false。
完整截图和报告已更新至 evidence 下四个对应目录；失败日志继续保留于 review-fix。

## 发布门禁

1. 固化最终本地精确 HEAD，由未参与实现的独立 Review Agent 重新全面审核。
2. 只有新报告明确 APPROVED、HEAD 完全相同且最新远端祖先关系安全时才正常推送。

发布资格仅由当前精确 HEAD 对应的独立 APPROVED 决定，不能从本文自述推定。
最终审核和发布记录保留提交外，避免提交身份漂移。已有旧 UX-R3 未跟踪材料保留。
