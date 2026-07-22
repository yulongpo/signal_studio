# MS-00 依赖报告

`dependencies/dependency-lock.json` 将不可变已批准输入与本机检测证据分离。两个校验器均加载只读 BL1.0 依赖来源，并比较每个选定元组：名称、包含 port version 的不可变版本、SPDX、官方来源 URL、vcpkg 锁引用和 baseline 验证值。

## 锁定依赖状态

- vcpkg builtin baseline：`82b6bc886d7b0f8342e34babc2e0b8943f79b0e1`
- 已批准获取脚本归档：`https://github.com/microsoft/vcpkg/archive/82b6bc886d7b0f8342e34babc2e0b8943f79b0e1.tar.gz`
- 归档 SHA-256：`550800632708a561c82412ee69e227c261d0ac8bc381eee09d123014528ae97a`
- 归档大小：5,332,790 字节
- baseline JSON SHA-256：`9f3b13f9a142969a043a921f544c637c54be46b06eabb9025b7e5c28b908af58`
- 选定 ports：qtbase、qttools、intel-mkl、eigen3、tbb、hdf5、nlohmann-json、tomlplusplus、pybind11、onnxruntime、spdlog、fmt、gtest、benchmark

不可变获取脚本、其依赖锁和实时流式下载三者一致：`.tar.gz` 为 5,332,790 字节，SHA-256 为 `550800632708a561c82412ee69e227c261d0ac8bc381eee09d123014528ae97a`。GitHub API 将所请求完整提交解析为相同提交 `82b6bc886d7b0f8342e34babc2e0b8943f79b0e1`，归档根目录为 `vcpkg-82b6bc886d7b0f8342e34babc2e0b8943f79b0e1/`。对照下载的 `.zip` 为 11,349,427 字节，SHA-256 为 `6e63222e536d62a0f26fc6070bce694a3d9e0d42dc0a43d07802500992f08b`；它不是 BL1.0 已批准字节流，依赖锁不接受该表示。

BL1.0 未定义每个 port 的源归档 SHA-256。每个空包哈希因此必须携带 `not-defined-by-bl1.0` 状态，校验器会拒绝没有解释的空值。未虚构任何包哈希。

## Qt 版本语义

不可变 BL1.0 选择 qtbase 6.11.1#1 与 qttools 6.11.1，项目依赖锁逐项保留这些版本；本机检测证据也是 Qt 6.11.1。二者都不自动定义源码最低版本。当前 Visualization/Workbench 的私有 Qt 使用只依赖 Qt 6 通用版本宏，远程 CI 已在 Qt 6.10.3 完成安装、配置并进入实际编译，因此项目将 6.10.3 作为经过验证的最低支持版本。

`qt_compatibility_contract` 分别记录最低支持 6.10.3、CI 验证 6.10.3、本机验证 6.11.1 和 BL1.0 两个 port 选择。Python/PowerShell 依赖校验器会交叉检查这些字段，既不能篡改不可变 BL1.0 版本，也不能把本机 6.11.1 误当成源码最低版本。

## 已安装实例证据与获取契约

Git、CMake、Ninja、Qt/qmake、MSVC/cl、Windows SDK/rc 和 Python 记录检测到的精确版本、可执行文件路径、已安装文件 SHA-256 与哈希范围。这些哈希标识本机已测文件，不是厂商安装包哈希。

BL1.0 仅为 vcpkg 归档和可选 CUDA 12.8.1 网络安装器提供获取 URL/SHA-256。Visual Studio/Windows SDK 使用显式 `channel-managed-not-defined-by-bl1.0`；Git、CMake、Ninja、独立 Qt 和 Python 使用 `not-defined-by-bl1.0`。这些状态要求获取 URL/哈希为空并提供策略说明。CUDA 在本机显式记录为未检测到。

## 离线材料与初始化行为

预期缓存当前缺少已批准 vcpkg `.tar.gz` 与可选 CUDA 安装器。两者都有 BL1.0 已批准 URL 和 SHA-256，且都不是无第三方依赖 MS-00 契约构建的前置条件。材料出现时，初始化会在使用前校验哈希和已知大小。两个校验器会解析不可变获取脚本、展开提交 URL/文件名，并拒绝任何不同的锁或缓存表示。

在同一进程连续执行两次初始化返回相同缺失列表，未修改系统状态，PATH 保持全部条目唯一。最终次数、长度和用户预设字节数以本轮证据日志为准。
