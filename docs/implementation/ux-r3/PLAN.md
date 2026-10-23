# UX-R3 执行计划

基线核验（2026-10-23）：`main` 为 `36a509d56764457524faa72d9af6ff95429bcb1c`，fetch 后 `origin/rebuild/a143-native` 为 `90be58e452f17b121028efc8b34af9be50a76e6e`；文件树一致，前者保留额外合并历史。本地功能分支 `feature/ux-r3-optimization` 从当前 HEAD 建立。用户原有未跟踪文件全部保留。

执行依据为 `reference/Signal_Studio_UX_R3_Codex_Master_Prompt.md` 全文。G0 核对代码/显示器/构建身份；G1 冻结交互合同并提供可点击预览；G2 工程文件与显式迁移；G3 动态轴布局；G4 标签发现/映射；G5 独立窄带页；G6 检测及树联动；G7 Debug/Release、实屏及独立部署，随后独立终审。

改动范围：`domain/project.h`、`infrastructure/project_store.*` 和工程文件 codec/resolver、`infrastructure/label_import.*` 与 discovery、`ui/charts/plot_widget.*` 与 axis layout、`ui/narrowband_workspace.*`、`app/main_window.*`、相关行为测试、`scripts/` 验证/文件关联及本目录文档。现有 DSP/worker 与来源修订契约保留。

节点中间证据位于 `out/tasks/wideband-detection/UX-R3/`；必要交付证据归档本目录。原生探针已确认连接 2、Redmi 27 NU、DISPLAY6、3840×2160、150%、Qt 2560×1440。不得使用其它分辨率替代指定实屏。

发布门禁：允许本地阶段提交；最终真实独立 Review Agent 审查精确 HEAD 并输出 `INDEPENDENT_REVIEW.md`。仅 APPROVED 且当前 HEAD 相同、最新远端为安全祖先时正常推送 `rebuild/a143-native`。拒绝/阻塞时修复复审；不向 main 或其它远端分支发布，不强推。
