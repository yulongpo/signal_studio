import { appState, nowTime, taskStatusText } from './state.js';

let timer = null;
let listeners = [];

function notify() { listeners.forEach((listener) => listener()); }
function task(id, name, type, duration = 1800) { return { id, name, type, status: 'queued', progress: 0, duration, startedAt: null, elapsed: '—', canRetry: false }; }

export function addLog(level, message, taskName = '') {
  appState.logs.unshift({ time: nowTime(), level, message, taskName });
  if (appState.logs.length > 80) appState.logs.length = 80;
  notify();
}

export function createTaskQueue() {
  listeners = [];
  return {
    subscribe(listener) { listeners.push(listener); return () => { listeners = listeners.filter((item) => item !== listener); }; },
    resetImportTasks() {
      appState.tasks = [task('read', '读取文件信息', 'I/O / metadata', 1100), task('summary', '生成时域概要', 'overview / coarse', 1500), task('fft', '计算频谱', 'FFT / 4096', 1900), task('stft', '计算 STFT', 'STFT / Hann', 2200), task('display', '更新显示', 'render / cache', 900)];
      appState.dataState = 'importing';
      appState.error = null;
      addLog('info', '已创建 RAW IQ 模拟导入任务队列。', '读取文件信息');
      notify();
      startNext();
    },
    startNext,
    pause() { const current = appState.tasks.find((item) => item.status === 'running'); if (!current) return; current.status = 'paused'; addLog('warning', `${current.name} 已暂停。`, current.name); notify(); },
    resume() { const current = appState.tasks.find((item) => item.status === 'paused'); if (!current) return; current.status = 'running'; addLog('info', `${current.name} 已继续。`, current.name); notify(); },
    cancel() { const current = appState.tasks.find((item) => item.status === 'running' || item.status === 'paused'); if (!current) return; current.status = 'cancelled'; current.canRetry = true; addLog('warning', `${current.name} 已取消，后续任务保留在队列中。`, current.name); notify(); },
    retry(id) { const target = appState.tasks.find((item) => item.id === id); if (!target) return; target.status = 'retrying'; target.progress = 0; target.canRetry = false; appState.error = null; addLog('info', `${target.name} 开始重试。`, target.name); setTimeout(() => { target.status = 'running'; startTimer(); notify(); }, 350); },
    failRead() { const current = appState.tasks.find((item) => item.id === 'read'); if (!current) return; current.status = 'failed'; current.progress = 35; current.canRetry = true; appState.error = { code: 'SS-IO-004', title: '模拟读取错误', detail: '分块读取返回了不可解析的字节序列。原型不会触碰真实文件。', suggestion: '请重试任务，或返回导入向导选择其他文件。' }; appState.dataState = 'error'; addLog('error', '读取文件信息失败：SS-IO-004 / simulated byte sequence.', current.name); notify(); stopTimer(); },
    clearCompleted() { appState.tasks = appState.tasks.filter((item) => !['completed', 'cancelled'].includes(item.status)); notify(); },
    clearLogs() { appState.logs = []; notify(); },
    getCurrent() { return appState.tasks.find((item) => ['running', 'paused', 'retrying'].includes(item.status)); }
  };
}

let queueApi;
function startNext() {
  const next = appState.tasks.find((item) => item.status === 'queued');
  if (!next) { if (appState.tasks.length && appState.tasks.every((item) => item.status === 'completed')) { appState.dataState = 'ready'; appState.runtime.status = '宽带工作区就绪'; addLog('info', '宽带浏览工作区已就绪，显示高分辨率模拟结果。', '更新显示'); } notify(); stopTimer(); return; }
  next.status = 'running'; next.startedAt = Date.now(); appState.runtime.status = next.name; addLog('info', `${next.name} 开始。`, next.name); startTimer(); notify();
}
function startTimer() { stopTimer(); timer = window.setInterval(() => {
  const current = appState.tasks.find((item) => item.status === 'running');
  if (!current) { if (appState.tasks.some((item) => item.status === 'queued')) startNext(); return; }
  current.progress = Math.min(100, current.progress + Math.max(3, 1000 / current.duration * 8)); current.elapsed = `${((Date.now() - current.startedAt) / 1000).toFixed(1)} s`;
  if (current.progress >= 100) { current.status = 'completed'; current.canRetry = false; current.elapsed = `${(current.duration / 1000).toFixed(1)} s`; addLog('info', `${current.name} 完成。`, current.name); startNext(); }
  notify();
}, 160); }
function stopTimer() { if (timer) { window.clearInterval(timer); timer = null; } }

export function setTaskQueueApi(api) { queueApi = api; }
export { taskStatusText };
