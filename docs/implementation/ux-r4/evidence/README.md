# 原生与回归证据

- before/01-default.png：改动前实际默认窗口初始帧。后续 Windows grabWindow 滞留画面已排除。
- release-gpu、release-software、debug-gpu、debug-software：每组 21 个 4K 主窗口 PNG、
  1 个原生详情窗口 PNG 和 report.json。报告保留当次原始 out 路径；同名 PNG 完整归档在此。
- logs：原始构建、CTest、Qt 定向及阶段失败日志。多次构建/补测分开记录，不能混称一次全过。
- package：两配置、两 renderer 的独立应用启动报告。
- diagnostics：修复前失败报告、旧截图采集资格说明及一次软件延迟时戳记录。
- review-fix：补充来源修复、系统剪贴板阻塞及用户恢复后的证据。
- search-fix：最终重复词跨页/CRLF 小型回归、构建和独立启动证据。四个主目录为此轮最终原生截图/报告。

报告中的源路径和运行路径是当次本机事实；重新验证使用 scripts/verify-ux-r4.ps1 并指定新的绝对目录。
未对这些文件批量计算指纹。Qt 产品自身必要的标签/工程完整性校验保持开启。
