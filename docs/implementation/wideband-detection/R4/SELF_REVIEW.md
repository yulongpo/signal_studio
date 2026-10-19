# R4 自检与复审

日期：2026-10-15。前置R3提交`a49b839`；分支`rebuild/a143-native`。本节点实现、自检、复审通过，本地提交后才进入R5，尚未推送。

| 检查 | 实际结果 | 证据 |
|---|---|---|
| 获批M0-P资源身份 | 22/22一致 | `prototype-identity.json` |
| 既有脚本Release构建 | 通过 | `out/tasks/wideband-detection/R4/logs/release-build-final-r2.txt` |
| 既有脚本完整CTest | 23/23，103.01秒 | `release-all-tests.txt` |
| Qt offscreen UI | 94/94，0跳过，74.074秒 | `release-ui.txt` |
| 原生生产旅程 | 43/43，47.950秒 | `native/report.json`及43个原始PNG |
| 缺模型/进程隐藏CUDA设备 | 2/2，标签/手动/历史仍可用 | `availability/`的JSON与原始PNG |
| 格式、PowerShell语法、diff及源码身份 | 通过 | `input-snapshot.json` |
| 最终只读复审 | PASS，无剩余P1/P2 | I12可见性及文本区可读性均补强 |

原生实际连接屏2为Redmi27NU / DISPLAY6，3840×2160物理、2560×1440逻辑、150% DPI、全屏。三图均实际提交硬件QRhi D3D11帧，AMD Radeon880M；主图38次纹理上传。真实TensorRT推理与真实CPU DDC另见`productionModel`，夹具的3条未知时间观察明确标为非模型，未冒充算法效果。

主Agent目视对照M0-P的`review-evidence.png`、`roi-confirm.png`与本节点10、24、43原始截图：三图、常驻配置、统一记录、三来源色、ROI确认、原始/当前证据及固定运行操作区保持；真实纹理、实际测量替代原型合成演示。生成器文本量化、10对象多段、幂等及重开均有直接断言。原始字段文本区在原位置展开，实际裁剪后至少95逻辑像素高、150像素宽；不是只检查隐藏文本或非空交集。

I01–I24逐项索引与用户明确批准的适配修订见`PROTOTYPE_CLOSURE.md`。三层分别关/开、三来源及真实run筛选直接核对可见行数，完整annotations/sourceSets/runs编码保持不变；同ID复核、拆合/恢复、导出与通道Changed快照均在原生旅程断言。R5综合验收标志在该节点完成前仍为false，不以本次原生旅程宣称规模或部署全部通过。

失败历史保留在R4任务目录：新增探针早期QStringLiteral拼接编译错误、确认复选框点击区域错误，以及原始字段零最小高度/文本光标滚动导致的裁剪。已分别修正，最终加强可读区域断言后重新通过43项。原型资源未修改；没有使用其它分支的产物。43截图与逐项SHA见`native/archive-index.json`。

本节点为少量真实IQ、功能与交互证据，不证明L3/L4、真实RF精度或大量数据效果通过。Debug/Release最终部署、启动/取消响应、十万活动段及原子发布稳定性进入R5后执行。
