# 节点状态

| 节点 | 状态 | 证据 |
|---|---|---|
| W0 | 完成 | 25 组双格式 fixtures；重建后的 DDC 基线 1/1；见 BASELINE.md |
| W1 | 完成 | 采样规划、真实 CI16 数值输出、v6 兼容；已有 4 项定向回归通过；新增 rate 1/1、Qt UI 4/4 |
| W2–W8 | 未开始 | 按依赖顺序执行 |

不推送远端；仅在节点自检通过后本地提交。保留失败和中断日志。

W1：`NB-W1/logs/directed-Debug.txt` 保留首轮新增夹具缺文件名造成的保存失败。
修正夹具后 `rate-fix.txt` 通过（含实际 CI16 tone 的 Fs/频率/幅值），
`ui-create.txt` 与构建树 `nb-analysis-ui-debug.txt` 通过（4/4，0 skip，7.537 秒）。
offscreen Qt 测试不证明 GPU 或实屏验收。
