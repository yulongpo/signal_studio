# SS-DE-RPO · Agent G 接口请求

G 不改 `CMakeLists.txt`。请主代理在 `if(SS_BUILD_TESTS)` 内、靠近现有 detection task 测试处加入下面两段。逻辑测试链接 `SignalStudioInfrastructure`（已 PUBLIC 链接 `SignalStudioCore` 与 `Qt6::Core`）。当前 UI 文件不是 widget 测试，不要加 `${SS_UI_SOURCES}`。

```cmake
    add_executable(SignalStudioSsDeRpoLogicTests tests/ss_de_rpo_logic_tests.cpp)
    ss_target_defaults(SignalStudioSsDeRpoLogicTests)
    set_target_properties(SignalStudioSsDeRpoLogicTests PROPERTIES AUTOMOC OFF)
    target_link_libraries(SignalStudioSsDeRpoLogicTests PRIVATE SignalStudioInfrastructure)
    add_test(NAME signal_studio_ss_de_rpo_logic COMMAND SignalStudioSsDeRpoLogicTests)
    set_tests_properties(signal_studio_ss_de_rpo_logic PROPERTIES TIMEOUT 60)

    add_executable(SignalStudioSsDeRpoUiTests tests/ss_de_rpo_ui_tests.cpp)
    ss_target_defaults(SignalStudioSsDeRpoUiTests)
    set_target_properties(SignalStudioSsDeRpoUiTests PROPERTIES AUTOMOC OFF)
    add_test(NAME signal_studio_ss_de_rpo_ui COMMAND SignalStudioSsDeRpoUiTests)
    set_tests_properties(signal_studio_ss_de_rpo_ui PROPERTIES TIMEOUT 30)
```

`ss_target_defaults` 在 MSVC 上打开 `/utf-8`。UI SKIP 字符串含「准备检测中」，需要这个选项。这两条用例不调用 oneMKL，不必 `ss_stage_mkl_runtime`。

## C 落地之后才替换 UI 目标

现在不要使用下面这段。仅当 `tests/ss_de_rpo_ui_tests.cpp` 改成真实 `DetectionCard` 测试、并调用已存在的 `startLatchActive()` 之后，删掉上面的 console UI 目标，改成与 `SignalStudioUxTests` 相同的 UI 源链接：

```cmake
    add_executable(SignalStudioSsDeRpoUiTests tests/ss_de_rpo_ui_tests.cpp ${SS_UI_SOURCES})
    target_link_libraries(SignalStudioSsDeRpoUiTests PRIVATE
        SignalStudioCore SignalStudioInfrastructure SignalStudioBrand Qt6::Widgets Qt6::Test
        Qt6::GuiPrivate Qt6::OpenGL Threads::Threads)
    ss_target_defaults(SignalStudioSsDeRpoUiTests)
    target_compile_definitions(SignalStudioSsDeRpoUiTests PRIVATE SS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    ss_add_graphics_shaders(SignalStudioSsDeRpoUiTests signalstudio_ss_de_rpo_ui_shaders)
    ss_add_ui_assets(SignalStudioSsDeRpoUiTests)
    add_test(NAME signal_studio_ss_de_rpo_ui COMMAND SignalStudioSsDeRpoUiTests)
    set_tests_properties(signal_studio_ss_de_rpo_ui PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5" TIMEOUT 120)
```

## 已有契约，不新造符号

- C：`DetectionCard::startLatchActive()` 与按钮文案「准备检测中」已写在 M1 契约第 4 节。`submit()` 在基线上是 private；双击应走 `runWidebandDetection`。空的 `projectDirectory` 会在 `sourceValidationError` 返回，计数会停在 0，不能当成闩锁通过。
- B：R-01 依赖 `contextValid` 不再因 `memoryGrantBytes > 1 GiB` 拒绝。G 不在测试里定义生产常量。
- D：R-04/R-05/R-12 使用现有 `applyDetectionEvidencePreflight` 与 `preserveDurableModelEvidence`。没有新请求字段。

基线上逻辑测试预期非零（R-01）。请不要为了让 CTest 变绿而改断言。UI SKIP 返回 0，在 C 替换成真实点击测试之前不要把它报成 R-08 通过。
