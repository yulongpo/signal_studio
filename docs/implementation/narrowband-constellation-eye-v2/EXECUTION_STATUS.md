# 节点状态

| 节点 | 状态 | 证据 |
|---|---|---|
| W0 | 完成 | 25 组双格式 fixtures；重建后的 DDC 基线 1/1；见 BASELINE.md |
| W1 | 完成 | 采样规划、真实 CI16 数值输出、v6 兼容；已有 4 项定向回归通过；新增 rate 1/1、Qt UI 4/4 |
| W2 | 完成 | 连续 IQ、精确卷积支持区间、uint64+小数来源映射、取消与并发 LRU；Debug 定向 3/3 |
| W3–W8 | 未开始 | 按依赖顺序执行 |

不推送远端；仅在节点自检通过后本地提交。保留失败和中断日志。

W1：`NB-W1/logs/directed-Debug.txt` 保留首轮新增夹具缺文件名造成的保存失败。
修正夹具后 `rate-fix.txt` 通过（含实际 CI16 tone 的 Fs/频率/幅值），
`ui-create.txt` 与构建树 `nb-analysis-ui-debug.txt` 通过（4/4，0 skip，7.537 秒）。
offscreen Qt 测试不证明 GPU 或实屏验收。

W2：`NB-W2/logs/` 保留首次脚本单元素数组被 PowerShell 展开为字符串的构建入口失败；
修正为显式 string[] 后最新定向日志 3/3（3.06 秒）。覆盖跨 16384 块与直接 DSP 的
1e-6 数值界限、双线程缓存、变 Fs/版本、>2^53 来源位置、溢出、Partial、预算和取消。
保留原 IQ 缓存指纹；FIR 计算移出缓存互斥锁，避免长 DSP 占锁阻塞另一读取器。
