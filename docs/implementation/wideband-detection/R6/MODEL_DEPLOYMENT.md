# 模型与本机部署

当前交付为Windows x64、TensorRT10.11.0.33/CUDA12.9、RTX5060Laptop（SM12.0）验证的包。Qt显示可用AMD880M D3D11或软件回退，与NVIDIA模型推理分别成立。模型、runtime、checkpoint和数据不随Git提交；只有源码、契约、许可证处理、身份及验证证据进入仓库。Git clone本身不能运行正式检测。

## 已验证的本机输出

`out/vs2026-qt611-debug_bin/`及`out/vs2026-qt611-release_bin/`均已部署。本次二进制在R5自检后未重建，R6只更新文档与CPU身份工具；源码/二进制身份以[R5自检](../R5/SELF_REVIEW.md)及两个`package-*-identity.json`为准，而非将可执行内部构建revision当最终文档提交号。

| 文件/条件 | 当前值 |
|---|---|
| worker | 输出目录下`SignalStudioDetectionWorker.exe` |
| FFSCN目录 | `detection-models/ffscn-gui-iq-v1/` |
| FFSCN活动文件 | `manifest.json`（schema v2）及`ffscn_epoch1200_dynamic_fp32.engine` |
| FFSCN engine SHA256 | `4e14e422e7966eb6f554e322c2d390534650e1b8dd0cee6e04f2ed4305cefc03` |
| FFSCN manifest SHA256 | `776ebd3261c063567e443f5b96d5c93a8e50449d9d491bb1242f8c7bf2575786` |
| SCN目录 | `detection-models/scn-max-psd-v1/` |
| SCN活动文件 | `manifest.json`（schema v1）及`scn_model.engine` |
| SCN engine SHA256 | `ae89d04332758d466846b71afc849c446500a82a8bd74fd8c671927247746627` |
| SCN manifest SHA256 | `689ca063995ef7f77dc8f8ec9189cdf55e616250b4278873b0edadee0a46192a` |
| 推理DLL | 输出目录下`nvinfer_10.dll`、`cudart64_12.dll`，分别校验冻结字节 |
| 许可证 | `detection-models/LICENSE.TensorRT.txt`、`LICENSE.CUDA.txt` |

FFSCN的profileId继续叫`ffscn-gui-iq-v1`，它是模型/权重身份，不等于manifest格式版本。原静态engine可留作历史数值回归，当前活动manifest指向动态engine；不能只看目录中是否有某个engine就判定实际使用版本。SCN的名称`scn-max-psd-v1`保留历史身份，也不限制当前/平均/最大/最小谱的选择。

主FFSCN/SCN均按用户确认的11层下采样校验模型输入，频率长度为2次幂且至少2048；SCN固定32768规格通过内部适配满足，原始SCN模型图缺失，未重新导出。具体张量、动态profile与资产边界见[R2](../R2/INPUT_ADAPTATION.md)和[SCN能力](../R2/SCN_CAPABILITY.md)。兼容GPU、runtime、包hash、布局和内存仍要真实有效；模型不可用不影响历史标记、原始标签或手动记录。当前没有CPU推理后端。

## 从已有冻结资产重新部署

在仓库根执行，先用`model_package.py`部署原基础包、SCN、DLL及许可证，再用动态包覆盖**FFSCN的manifest和活动engine**。顺序不可颠倒，否则基础包会把FFSCN恢复为旧静态规格。此过程只复制并校验已存在资产，不构建或启动模型。`stage-only`的checkpoint/runtime参数保留CLI形状，不读取训练仓库运行推理。

```powershell
$modelPython = 'D:/profiles/anaconda/python.exe'
$modelRuntime = 'out/tasks/wideband-detection/M3/assets/runtime/tensorrt-10.11'
New-Item -ItemType Directory -Force out/tasks/wideband-detection/R6/diagnostics | Out-Null
foreach ($modelConfiguration in @('debug', 'release')) {
  $modelBin = "out/vs2026-qt611-${modelConfiguration}_bin"
  & $modelPython -B tools/detection/model_package.py `
    --asset-root out/tasks/wideband-detection/M3/assets `
    --runtime-root $modelRuntime --output-bin $modelBin `
    --report "out/tasks/wideband-detection/R6/diagnostics/base-${modelConfiguration}.json"
  if ($LASTEXITCODE -ne 0) { throw 'Base model deployment failed' }
  & $modelPython -B tools/detection/model_dynamic_package.py `
    --checkpoint D:/project/FFSCN_train2/FFSCN_TEST_GUI/config/epoch_1200.pth `
    --trt-root $modelRuntime `
    --output out/tasks/wideband-detection/R2/assets/dynamic-ffscn `
    --report out/tasks/wideband-detection/R2/diagnostics/dynamic-model-probe.json `
    --stage-only --output-bin $modelBin
  if ($LASTEXITCODE -ne 0) { throw 'Dynamic FFSCN deployment failed' }
}
```

复制完成后检查两份活动manifest/engine身份，再从输出目录运行。Qt/CRT由`scripts/build.ps1`完成，模型工具不代替编译或Qt部署。应用运行无需Python、SDK路径和训练目录，原始文件路径及检测附属记录仍需存在。公开CMake仅部署Qt/CRT，不会联网下载模型。

需要重新导出FFSCN时，使用`tools/detection/model_dynamic_package.py --help`和[R2完整命令](../R2/DYNAMIC_MODEL.md)，将`--output`及`--report`改到新的`R6/assets/`和`R6/diagnostics/`目录，保留冻结epoch1200权重身份。工具会导出ONNX、构建engine、执行5组PyTorch/head对照；不是仅复制。默认两档资源范围与当前包一致。重建后重新验证并建立新身份，不覆盖历史R2结论。其它GPU需独立适配与验证，不能承诺当前序列化包通用。
