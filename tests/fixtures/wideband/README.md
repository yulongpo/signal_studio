# 宽带标签契约夹具

这些是用户指定真实标签文件的原样文本副本，2026-10-13 加入，用于坐标/格式与工程状态测试；不是检测预测或算法效果报告。不提交原始 IQ 负载，运行数据仍由资产 manifest 明确绑定。

`.gitattributes` 对这些来源夹具关闭文本换行转换，生成器TXT保留writer原有行尾空白，以维持逐字节身份；普通文档/代码仍执行正常空白检查。

| 文件 | 来源 | 用途 |
|---|---|---|
| torchsig-0000000.sigmf-meta | `W:/wbSig53_rfbench20m/train/baseline/0000000.sigmf-meta` | 20Msps、2.5GHz、400000样本、4个绝对RF事件 |
| torchsig-0000000.json | 同目录 `0000000.json` | 同源rfbench-v1扩展、样本半开区间和相对频率核验 |
| generator-60m-10ms.txt | `D:/coding/FFSCN_train2/broadband_signal_generation/figures/examples_60MHz_10ms_20260906/sparse/label/20260906-213604-798_914585_bw_60000000_sampleTime_0.01_rollOff_0.35.txt` | 带键TXT、每载波多TIME段、文本量化/名义BW |

TorchSig meta 中频率已是 RF，不能再加 Fc。TXT 的 FC 是生成坐标，需减 Fs/2 后按来源中心频率解释，TIME 五位小数不作为样本级真值。M0仅校验结构与源字段，真实标签解析器及原生导入在M1实现。
