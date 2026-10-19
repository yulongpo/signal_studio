# 标记交互 U0 审核原型

本轮用户方案的局部可点击原型，设计工作区 **2560×1440 逻辑像素**。沿用 Signal Studio 深色工程树、三图、右侧详情、底部表格结构。它只演示交互；全部图谱、标记和通道为合成演示，不读取 IQ、不运行模型或 DDC。

从仓库根目录运行：

```powershell
& 'D:/profiles/anaconda/python.exe' -m http.server 8785 --bind 127.0.0.1 --directory D:/project/signal_studio_rebuild
```

打开 [本机审核页面](http://127.0.0.1:8785/docs/prototype/annotation-interactions-u0/index.html)。页面使用原生 ES modules，需要 HTTP 服务，不能直接双击 HTML。无安装步骤、第三方运行依赖、CDN 或联网字体。品牌资源只引用本仓库 `resources/branding/`。

## 建议审核路径

1. 默认选中“ALG 02 · 时间未定位观察带”。从图、表、左树选择其它对象；Ctrl 多选、Shift 连选，观察来源色、当前对象和悬停。单击不自动缩放，定位按钮/树双击才调整视区。
2. 选择 Manual 或 ALG 01 的第 2 段，核对表格范围为当前段；只有当前段显示控制点。三来源均可拖动/调整边界，或双击/F2 编辑主表；右侧“完整范围编辑”一次提交全部范围。原始证据保留。
3. 改名/备注、质量标记后查看已有 ALG 01 通道仍“范围一致”；改几何后恢复待判断、通道显示差异；撤销恢复。质量菜单为可选标记，不阻止通道创建。
4. 多选→删除所选，核对对象/段/通道数；确认后关联通道保留并提示来源已删除；一次撤销。右侧“删除当前段”有独立名称，最后一段显示删除对象说明。
5. ALG 02→创建通道，检查 UserRange 默认视区∩可用前缀、不生成活动段；已定位结果默认当前段，可裁剪。切换 Partial/未读尾部场景；空交集保留错误，只有显式“使用可用前缀”才改范围。来源改变/不可读场景保留具体错误。
6. 260+ 场景全选匹配→分页、排序、来源过滤/图层开关，检查跨页保留及过滤后剔除。左树按需加载，图选/表选会定位并加载对应项。编辑草稿期间点“追加结果演示”，检查草稿和选择保持；Enter 提交、Esc 取消。
7. 保存演示→重置→重开演示，检查修订、质量、删除和两类通道快照。保存/导出是独立 `demoOnly:true` v6 浏览器状态，**不是原生工程 v6**。

原始四条标签几何常量与 M0-P 记录一致；能量图仍是合成纹理。其余对象由本原型独立生成。大场景仅为 268 个合成 UI 对象，不是大量数据效果验证。

## 交付与验证

[相对 M0-P 的修订](../../implementation/annotation-interactions/U0/M0P_DELTA.md) · [U0 验证记录](../../implementation/annotation-interactions/U0/VALIDATION.md) · [实施节点](../../implementation/annotation-interactions/PLAN.md)。冻结 M0-P 保持不动。本原型确认后才进入 U1–U4 Qt 实现与原生回归。

自检入口为 `scripts/verify_annotation_interaction_model.mjs` 和 `scripts/verify_annotation_interaction_prototype.mjs`，后者复用项目现有 Playwright/Edge 浏览器检查方式。中间结果在 `out/tasks/wideband-detection/U0/`，最终报告和截图归档对应 U0 目录。浏览器测试不证明 Qt、指定第二屏 DPI、GPU 推理或真实 CPU DDC。
