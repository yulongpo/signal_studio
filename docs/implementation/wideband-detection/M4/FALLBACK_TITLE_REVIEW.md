# M4 回退卡文案补审

预退出 SPEC 发现：旧标题“精修回退 · 保留原始支持范围”对所有 Fallback 作相同描述，但 Fine 失败后可以使用独立实测的 Coarse 活动，不能保证与原模型观察范围相等。

修正仅将标题改为中性“精修回退”。卡片继续显示保存的时间依据、原始/当前范围和完整回退原因；无活动结果仍显示时间未定位，实测粗回退保留 measured-coarse-fallback 及对应证据。保持获批卡片布局、动作和参数矩阵，不改变任何算法、坐标或固定评价输入。

改动保存在 M4 deferred/evidence-r5；它由 r4 七文件复制，仅修改证据卡这一条标题。隔离候选已复制对应新文件。R3 冻结数值版本未变，旧候选 23/23 的结果保留其原源码身份；本次构建和相关检查另留记录，最终主目录完整自检和原生检查仍待执行。

r5 七文件与候选逐字节一致，身份见 evidence-r5-draft-identity.json。SPEC 复核确认其余六文件未变，标题缺口关闭；证据卡文件 SHA-256 为 9b3611ce0f1e818692e5461d22ca08da71e4c697b452b3047c90e2549a16e49e。候选自身 scripts/build.ps1 Debug Build 通过，既有 widebandFixtureTaskNativeRoundtrip 通过（含 init/cleanup 共 3/3、4996 ms），日志见 fallback-title-focused-ui-r5.txt。该检查覆盖证据卡和原生文件持久化交互，采用 offscreen 软件显示，不作为 GPU、效果或新原生截图证据。
