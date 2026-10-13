# M0 验证记录

日期：2026-10-13。范围为契约、参考资产身份和环境探针；未构建应用、未加载或执行模型、未运行历史效果评估。

`scripts/validate_wideband_contracts.py` 使用 Draft 2020-12 校验器核验3个schema，验证4条用户指定SigMF标签及配套JSON的样本/频率一致性，检查有效run/握手，并拒绝22个溢出、倒序、伪分数、非有限值、越界、路径、消息关联与二进制长度反例。实际输出见 `contract-validation.txt`。

独立审查发现的Windows根相对路径（`/outside.json`、`\outside.json`）以及原始扩展字段/参数内的无穷值已修正并加入反例。JSON读取也检查有限浮点值，避免合法数字文本`1e999`被解码成无穷值。

质量审查发现普通正则`$`可能接受尾换行，已将样本/hash的schema末端改为严格末端断言，语义层同时采用完整匹配；样本、序号和hash的尾换行反例均已加入。

工具环境仅建于被忽略的 `out/wideband-tools/`，没有修改系统或已有Anaconda依赖。完整版本记录在 `tools-lock.txt`，校验器版本和支持标准依据 [jsonschema 官方文档](https://python-jsonschema.readthedocs.io/en/stable/)。重现命令：

```powershell
python -m venv out/wideband-tools
./out/wideband-tools/Scripts/python.exe -m pip install -r docs/implementation/wideband-detection/M0/tools-lock.txt
./out/wideband-tools/Scripts/python.exe scripts/validate_wideband_contracts.py
```

资产/设备验证另见 `ASSET_AUDIT.md` 与 `assets.json`。本轮实际确认模型身份、参考配置及10组样本manifest，GPU仅查询设备信息；TensorRT生产转换和原生运行属于M3。浏览器探针使用已安装的Microsoft Edge，后续原型验证使用2560×1440逻辑viewport，仅证明浏览器演示，不替代原生第二屏验收。

整体完成判断还包含独立规格审查和质量审查；审查结论及修正写入本目录的审查记录后再本地提交。
