export const signalConfig = {
  path: 'D:/data/sample_100ms_IQ.raw',
  dataType: 'Int16',
  iqOrder: 'IQ',
  endian: 'Little Endian',
  sampleRate: '250000000',
  sampleRateUnit: 'S/s',
  centerFrequency: '100000000',
  centerFrequencyUnit: 'Hz',
  headerOffset: '0',
  headerUnit: 'bytes',
  amplitudeScale: '1.0',
  startUtc: '2026-07-15T11:34'
};

export const appState = {
  view: 'welcome',
  importStep: 1,
  importOpen: false,
  importValidation: null,
  importConfig: { ...signalConfig },
  dataState: 'empty',
  selectedNode: null,
  dirty: false,
  panel: { left: false, right: false, bottom: false },
  bottomTab: 'tasks',
  graphMode: 'select',
  linked: true,
  themeAlt: false,
  chart: {
    time: { zoom: 1, offset: 0, selection: null, cursor: null },
    spectrum: { zoom: 1, offset: 0, selection: null, cursor: null },
    stft: { zoom: 1, offset: 0, selection: null, cursor: null }
  },
  selection: null,
  cursors: { M1: null, M2: null, C1: null },
  marks: [],
  tasks: [],
  logs: [],
  error: null,
  runtime: { cpu: 8, memory: 412, cache: 0, status: '就绪' }
};

export const taskStatusText = {
  queued: '排队中',
  running: '运行中',
  paused: '已暂停',
  completed: '已完成',
  cancelled: '已取消',
  failed: '失败',
  cancelling: '取消中',
  retrying: '重试中'
};

export const nodeTree = [
  { id: 'project', label: 'Signal Studio 项目', icon: '◆', expanded: true, type: 'project' },
  { id: 'file', label: 'sample_100ms_IQ.raw', icon: '▣', child: true, expanded: true, type: 'file' },
  { id: 'overview', label: '概览', icon: '◌', child: true, type: 'overview' },
  { id: 'time', label: '时域分析', icon: '∿', child: true, type: 'time' },
  { id: 'spectrum', label: '频谱分析', icon: '⌁', child: true, type: 'spectrum' },
  { id: 'stft', label: '时频分析', icon: '▦', child: true, type: 'stft' },
  { id: 'modulation', label: '调制分析（后续）', icon: '◇', child: true, disabled: true, type: 'future' },
  { id: 'measurements', label: '测量结果', icon: '⌖', child: true, type: 'measurements' },
  { id: 'marks', label: '标签与标记', icon: '⚑', child: true, type: 'marks' }
];

export function resetCharts() {
  for (const key of ['time', 'spectrum', 'stft']) {
    appState.chart[key] = { zoom: 1, offset: 0, selection: null, cursor: null };
  }
  appState.selection = null;
  appState.cursors = { M1: null, M2: null, C1: null };
}

export function formatNumber(value, digits = 1) {
  return Number(value).toLocaleString('en-US', { maximumFractionDigits: digits, minimumFractionDigits: digits });
}

export function nowTime() {
  return new Date().toLocaleTimeString('zh-CN', { hour12: false });
}
