#pragma once

#include <QString>
#include <string>

namespace signalstudio {
inline QString detectionPhaseText(const std::string& phase) {
  // preparing/starting are UI latch labels. preflight, worker-init, and
  // publishing name phases the task already records or the lead may pass.
  if (phase == "preparing" || phase == "starting") return "准备检测中";
  if (phase == "preflight") return "正在检查来源/模型/资源";
  if (phase == "worker-init" || phase == "initializing") return "初始化 Worker";
  if (phase == "reading") return "读取 / 谱帧";
  if (phase == "inference") return "模型前向";
  if (phase == "decoding") return "模型输出解码";
  if (phase == "storing-heads") return "原始模型输出落盘";
  if (phase == "tracking") return "窗口轨迹关联";
  if (phase == "checkpoint") return "证据分片 / 检查点提交";
  if (phase == "frequency") return "关联 / 频率处理";
  if (phase == "refine") return "时间精修";
  if (phase == "publishing") return "正在校验并发布正式结果";
  if (phase == "completed") return "已完成";
  return "暂无阶段记录";
}
}  // namespace signalstudio
