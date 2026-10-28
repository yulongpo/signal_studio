# SS-DE-RPO 续作验收矩阵

统一入口：`scripts/verify-ss-de-rpo.ps1`，最终证据 `out/tasks/ss-de-rpo/N4/final-acceptance-04/`。R-01～R-20 均 PASS。GPU、原始 IQ、合成 IQ、fixture、QRhi 和软件回退按各自证据层级记录；单个纯逻辑 PASS 不代表真实硬件 PASS。

| 用例 | 状态 | 证据范围 |
|---|---|---|
| R-01 | PASS | 默认 8 GiB 授权及生产 Sample 路径；原始 IQ 20 窗口 |
| R-02 | PASS | 1/2/4/8/16 GiB 下发与实际规划/执行预算边界 |
| R-03 | PASS | checked arithmetic、低 RAM/磁盘、连续分配及 cells 分离 |
| R-04 | PASS | raw-head OFF 公共请求额度与显式额度契约 |
| R-05 | PASS | 2044×196608 B 需求/不足拒绝；实际 20 窗口 raw-head ON |
| R-06 | PASS | 正式 SourcePlan 20/159/363/2044；没有复制帧 |
| R-07 | PASS | Sample/ROI/Coarse、Golden 及质量回退 |
| R-08 | PASS | 100 ms 内五次请求、一次 latch/run；原生启动反馈 |
| R-09 | PASS | 准备/初始化取消、提交前后竞争、退出与资源收尾 |
| R-10 | PASS | 损坏模型、来源改变、磁盘/工程提交失败与重试提示 |
| R-11 | PASS | 原始 IQ 159/363/2044 完整持久发布、比较和重开 |
| R-12 | PASS | durable/non-durable supplemental model evidence 分支 |
| R-13 | PASS | 接受启动清空旧 slot；失败取消不恢复；仅编辑参数保留 |
| R-14 | PASS | 坏 shard、项目提交失败、恢复和正式结果隔离 |
| R-15 | PASS | 真实 FFSCN/SCN、实际合法 profile；注入 B2→B1 同窗重试及 B1 失败/取消 |
| R-16 | PASS | 频率索引穷举 oracle、边界/包含/零 IoU、真过载原子失败 |
| R-17 | PASS | 原始大窗 + 合成 Golden/长信号；60 s 实际 Fs bounded 执行明确回退 |
| R-18 | PASS | 来源/前缀/通道/参数/资产/profile 变化的准备复用失效 |
| R-19 | PASS | 原生 4K/150% 20 次准备反馈；Paint dispatch 完成 p95 |
| R-20 | PASS | 工程、标记、窄带、PSD/STFT/瀑布完整回归；QRhi 与软件独立部署 |

Release 完整 CTest：60/60，298.99 s，一次全部通过。Debug：56 项在中断前通过，工作流原 180 s 超时，最后三项未完成；四项补测均通过，覆盖当前注册的 60 项。工作流 27 个 Qt 行完整通过 136.51 s；主 UI/参数/标记三组分别 109.40/338.81/234.38 s，保留 480 s UI 终止保护。`current-ctest-evidence/debug.json` 记录原失败、缺失与补测，重新核对产品/测试源码和构建定义。不能将 Debug 描述成一次完整全过。

原生反馈：Debug 7.63922 ms、Release 43.7656 ms，均 n=20，低于 100 ms。边界为 QWidget Paint dispatch 完成；handler、queued callback 独立保留，不包含 DWM/扫描输出。五次连点分别 37/7 ms，仅一个请求。实际屏幕 Redmi 27 NU、连接2、DISPLAY6、3840×2160、DPR1.5、逻辑全屏2560×1440。

原始 IQ 的大用例保存其实际执行版本，并经统一入口重新确认处理路径源码一致；不声称所有大用例在最终 UI 文档提交后重新运行。60 s/20 MS/s 合成持续输入完成有界执行与 scratch 归还，Sample publicationEligible=false，必须采用已记录的 measured-coarse fallback。该项不计作 Sample 精度或现场 RF 验收。

所有历史失败、超时和中断保留。最终独立 H 审查及远端发布事实以提交外记录为准。
