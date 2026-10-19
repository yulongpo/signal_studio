# R3 自检与复审

日期：2026-10-15。分支`rebuild/a143-native`，前置R2提交`e4db9c8`。实现详见`FLOW_ADAPTATION.md`。本节点自检、只读复审通过，本地提交后才进入R4；尚未推送。

| 检查 | 实际结果 | 证据 |
|---|---|---|
| 自身脚本Release构建 | 通过 | `out/tasks/wideband-detection/R3/logs/release-build-final.txt` |
| 自身脚本完整CTest | 23/23，86.65秒 | `release-all-tests.txt` |
| Qt offscreen UI | 94/94，0跳过，58.836秒 | `release-ui.txt` |
| 真实WAV代表流程 | 4/4，14.704秒 | `real-flows.txt`、`real-flows.json` |
| Google格式、diff检查、源码身份 | 通过 | `input-snapshot.json`、`runtime-snapshot.json` |
| 最终只读代码复审 | PASS，无剩余P1/P2 | 本文下述关闭项 |

真实WAV SHA-256为`1757d92722e78776a6877b4fe5f29734fd2975989aaef1d52f8103aaa310b18b`，20Msps生成器PCM16 IQ。仅处理20ms范围及17样本Partial前缀。FFSCN的8192点/3帧/2ms、SCN平均谱的3001点/2帧/3ms、SCN17样本短Partial、FFSCN正常零结果均完成、证据保存并精确重开；对象数分别0、13、4、0。这里只核对流程、有限输出和物理坐标，不评价检出精度。

来源/任务用例同时覆盖17样本边缘ROI、尾部槽、Partial未读排除、不规则FFT重算、实际1帧与uint32最大请求帧数的区分、4097真实帧映射保存重开、取消/失败恢复与不可变参数。UI直接核对非默认参数提交、平均/最小谱配置隔离、切换撤销草稿；这不是GPU显示验收。

复审关闭了uint32持久化截断、worker控制消息过大、旧4096映射限制、FFT时间标签与实际重算不符，以及单窗口预算遗漏原始head和SCN后处理JSON的问题。批量缩减与请求快照分开，必需输出和JSON格式预算均有具体报错。

失败历史保留在本节点logs：早期缺少投影声明导致编译失败，已补头文件；一次新增低采样率测试沿用过大合并距离，修正测试物理参数后通过。它们不是通过证据。R4原型/三来源原生核对及R5 Debug、部署、规模/稳定性尚待后续节点，L3/L4与真实RF效果仍未完整通过。
