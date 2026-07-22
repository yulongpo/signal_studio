# MS-00 实施报告

日期：2026-07-22

结论：Qt 最低版本一致性修复、本地全量自检及第三轮远程三项 CI 门禁通过；独立规范复审仍待再次核查

## 交付范围

- 保留用户授权净空前的恢复引用，并逐字节复制全部 136 个已批准 BL1.0 文件。
- 将 5,023,286,812 字节录制数据留在外部，同时记录精确路径、大小、SHA-256 和确定性小型夹具。
- 构建十个已批准 C++20 模块目标并强制精确公共 DAG。八个无界面模块无需 Qt；Visualization 和 Workbench 保持可选并私有链接 Qt。
- 将安装导出拆为无界面与 UI 目标集。两个包消费者分别链接、调用全部十个和八个可用模块；无界面组件消费不触发 Qt 发现。
- 加固公共 C11 ABI：定宽版本化 POD 表、可移植无抛出签名、稳定 C++ 全捕获适配器、C/C++ 无抛出/布局检查、真实 C 插件及故意抛异常的运行期封闭探针。
- 集中实现 BL1.0 Status 不变量：枚举域、稳定原因/类别映射、严重级别、重试/恢复规则和有界原因链。无效 Status 被工厂拒绝且不能序列化为 `unknown`。
- 增加机器可读依赖锁，将 14 个包元组与不可变 BL1.0 比较。已安装版本/路径/文件哈希与获取契约分离，缺失获取哈希使用明确策略而不是虚构值。
- 将 vcpkg 获取修正为 BL1.0 脚本规定的完整提交 `.tar.gz`。两个校验器直接解析脚本的提交、URL、SHA-256 和文件名，并与锁和离线清单交叉校验。
- 使 MSVC/Ninja/Qt 环境导入在单一 PowerShell 进程内幂等，并增加真实 Debug→Release 同会话构建/测试驱动。
- 重构被忽略的 `CMakeUserPresets.json`：唯一隐藏基预设保存一份规范化完整环境，别名仅继承；输出确定、压缩且原子替换。跨 PowerShell 7/Windows PowerShell 5.1 的重复生成结果为相同 8,774 字节和 SHA-256。
- 扩展到每个 UI 配置 41 个具名用例，包括十个独立模块性能保护和完整安装包覆盖。
- 增加便携 Windows/Linux 无界面 CI 和 Qt Windows CI。第二轮远程运行 `29918020386` 暴露 Visualization 遗留的 6.11 编译期断言；统一最低支持版本为 6.10.3 后，第三轮运行 `29919175820` 已针对提交 `d41c2748a465c4e843617e0a9444c8f8cc2f5015` 通过 Windows UI 模块/性能作业及 Windows、Ubuntu 两项无界面作业。本机验证和不可变 BL1.0 选择仍分别保留 6.11.1 证据，第三方 Action 均固定完整提交哈希。
- 在确认绝对目标位于仓库且不属于正式基线/外部清单/证据目录后，清理旧 `build/`、两个 Python `__pycache__`；本轮全新 Debug/Release 与本机预设配置验证完成并记录证据后，再次移除整个生成 `build/`，仅保留一份被忽略的干净用户预设。

## 环境与依赖状态

验证主机使用 Windows 11 build 26200、MSVC 19.44.35228/toolset 14.44.35207、Windows SDK 10.0.26100.0、CMake 4.3.1、Ninja 1.12.1、Qt 6.11.1、Git 2.53.0.windows.3 和 Python 3.13.12。精确可执行文件路径与 SHA-256 保存在 `dependencies/dependency-lock.json`，属于本机已安装实例证据，不是获取归档声明。

主机存在 RTX 5060 Laptop GPU 和驱动 591.84，但没有 `nvcc`/CUDA Toolkit。不宣称 CUDA 构建、数值或性能通过。经批准 CUDA 12.8.1 URL/SHA-256 仅作为可选离线材料保留，未获取。

BL1.0 的 vcpkg 获取契约为：提交 `82b6bc886d7b0f8342e34babc2e0b8943f79b0e1`，`.tar.gz` 5,332,790 字节，SHA-256 `550800632708a561c82412ee69e227c261d0ac8bc381eee09d123014528ae97a`。对照 `.zip` 为 11,349,427 字节、SHA-256 `6e63222e536d62a0f26fc6070bce694a3d9e0d42dc0a43d07802500992f08b`，因格式与字节不符而拒绝。缺少 vcpkg/CUDA 缓存不阻断无第三方依赖的 MS-00 平台构建。

## 验证命令

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/bootstrap.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/test-same-session.ps1 -Parallel 8 -SummaryOnly
python tests/platform/verify_manifests.py dependency-lock
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/validate-dependency-lock.ps1
python scripts/validate-ci-workflow.py
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/platform/verify_common_environment.ps1 -RepositoryRoot . -RequireQt
```

最低版本修复后同会话全新 Debug 41/41（CTest 19.79 秒），Release 41/41（CTest 17.42 秒）；总耗时分别为 33.684 秒和 26.395 秒，最终 PATH 为 52/52 唯一条目、长度 3139。两个配置均覆盖全组件安装包消费者和独立无 Qt 包消费者。旧运行失败、本地修复及第三轮远程成功证据均位于 `docs/milestones/MS-00/evidence/`。独立规范复审尚待再次核查，本记录不宣称代码质量评审通过；未启动 MS-01 或后续功能工作。
