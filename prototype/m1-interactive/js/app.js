import { appState, nodeTree, taskStatusText, formatNumber, resetCharts } from './state.js';
import { createRouter } from './router.js';
import { createWizard } from './import-wizard.js';
import { createCharts, getGraphReadout } from './charts.js';
import { createTaskQueue, addLog, setTaskQueueApi } from './tasks.js';

const root = document.querySelector('#app');
let charts;
let propertyDraft = {};
let menuOpen = false;
let toastTimer = 0;
const taskQueue = createTaskQueue();
setTaskQueueApi(taskQueue);

function esc(value) { return String(value ?? '').replace(/[&<>"']/g, (char) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#039;' }[char])); }
function stateClass(status) { return ['failed', 'error'].includes(status) ? 'error' : ['completed', 'success'].includes(status) ? 'success' : ['paused', 'warning'].includes(status) ? 'warning' : ''; }

const router = createRouter(appState, renderApp);
const wizard = createWizard({ onFinish: handleImportConfirm, onChange: (message) => { if (message) showToast(message, 'error'); }, onClose: () => {} });

function showToast(title, tone = 'info', detail = '') {
  const region = document.querySelector('#toast-region'); const item = document.createElement('div'); item.className = `toast ${tone}`; item.innerHTML = `<strong>${esc(title)}</strong>${detail ? `<span>${esc(detail)}</span>` : ''}`; region.appendChild(item); window.clearTimeout(toastTimer); toastTimer = window.setTimeout(() => item.remove(), 3400);
}

function renderTree() {
  const loaded = appState.dataState !== 'empty';
  const nodes = loaded ? nodeTree : [nodeTree[0]];
  document.querySelector('#project-tree').innerHTML = nodes.map((node) => `<div class="tree-row ${node.child ? 'child' : ''} ${node.disabled ? 'disabled' : ''} ${appState.selectedNode === node.id ? 'selected' : ''}" data-node-id="${node.id}" title="${node.disabled ? '后续版本功能' : `选择${node.label}`}" aria-disabled="${node.disabled ? 'true' : 'false'}"><span class="tree-caret">${node.expanded ? '⌄' : node.child ? '' : '›'}</span><span class="tree-icon ${node.type === 'stft' ? 'amber' : node.type === 'file' ? 'green' : ''}">${node.icon}</span><span>${node.label}</span>${node.disabled ? '<span class="future-tag">M2</span>' : ''}</div>`).join('') + (!loaded ? '<div class="tree-empty-note">导入 RAW IQ 后展开分析节点</div>' : '');
  const sourceRow = document.querySelector('#source-section .source-row');
  if (sourceRow) sourceRow.innerHTML = loaded ? '<span class="tree-icon green">✓</span><span>sample_100ms_IQ.raw</span>' : '<span class="tree-icon">◇</span><span>尚未加载数据</span>';
}

function renderInspector() {
  const body = document.querySelector('#inspector-body'); const title = document.querySelector('#inspector-title'); const node = nodeTree.find((item) => item.id === appState.selectedNode);
  if (!node || !appState.selectedNode || appState.dataState === 'empty') {
    title.textContent = '属性'; body.innerHTML = '<div class="inspector-empty"><div><div class="empty-glyph">◇</div><strong>未选择对象</strong><p>导入模拟数据后，在项目树中选择文件、图谱或分析节点查看属性。</p></div></div>'; return;
  }
  title.textContent = node.label;
  const common = `<div class="property-group"><div class="property-group-title"><span>对象信息</span><span class="status-chip success">已加载</span></div><div class="property-list"><div class="property-row"><label>类型</label><span class="property-value">${esc(node.type)}</span></div><div class="property-row"><label>状态</label><span class="property-value">${appState.dataState === 'error' ? '<span class="status-chip error">错误</span>' : '<span class="status-chip success">就绪</span>'}</span></div></div></div>`;
  if (node.type === 'file' || node.type === 'project') {
    propertyDraft = { fileName: propertyDraft.fileName || 'sample_100ms_IQ.raw', sampleRate: propertyDraft.sampleRate || '250 MSps', centerFrequency: propertyDraft.centerFrequency || '100 MHz', dataType: propertyDraft.dataType || 'Int16 / IQ', headerOffset: propertyDraft.headerOffset || '0 bytes', amplitudeScale: propertyDraft.amplitudeScale || '1.0' };
    body.innerHTML = `${common}<div class="property-group"><div class="property-group-title"><span>数据源参数</span><span class="dirty-note ${appState.dirty ? '' : 'hidden'}">● 未保存</span></div><div class="property-list"><div class="property-row"><label>文件名</label><span class="property-value"><input data-property-field="fileName" value="${esc(propertyDraft.fileName)}"></span></div><div class="property-row"><label>采样率</label><span class="property-value"><input data-property-field="sampleRate" value="${esc(propertyDraft.sampleRate)}"></span></div><div class="property-row"><label>中心频率</label><span class="property-value"><input data-property-field="centerFrequency" value="${esc(propertyDraft.centerFrequency)}"></span></div><div class="property-row"><label>类型 / 顺序</label><span class="property-value">${esc(propertyDraft.dataType)}</span></div><div class="property-row"><label>Header</label><span class="property-value">${esc(propertyDraft.headerOffset)}</span></div><div class="property-row"><label>缩放</label><span class="property-value"><input data-property-field="amplitudeScale" value="${esc(propertyDraft.amplitudeScale)}"></span></div></div><div class="property-actions"><button class="secondary-button compact" type="button" data-action="reset-properties">重置</button><button class="primary-button compact" type="button" data-action="apply-properties">应用</button></div></div><div class="property-group"><div class="property-group-title"><span>显示图层</span><span class="subtle">M1</span></div><div class="property-list"><div class="toggle-line"><span>I / 实部</span><label class="switch"><input type="checkbox" checked data-layer="i"><span class="slider"></span></label></div><div class="toggle-line"><span>Q / 虚部</span><label class="switch"><input type="checkbox" checked data-layer="q"><span class="slider"></span></label></div><div class="toggle-line"><span>平均频谱</span><label class="switch"><input type="checkbox" checked data-layer="average"><span class="slider"></span></label></div><div class="toggle-line"><span>STFT 颜色条</span><label class="switch"><input type="checkbox" checked data-layer="colorbar"><span class="slider"></span></label></div></div></div>`;
    return;
  }
  const graphInfo = node.type === 'time' ? [['时间范围', '0–100 ms'], ['振幅范围', '−60–0 dBFS'], ['统计', 'RMS / Peak / PAPR']] : node.type === 'spectrum' ? [['频率范围', '−125–125 MHz'], ['FFT', '4096 · Hann'], ['检测器', 'Peak / Avg']] : node.type === 'stft' ? [['时间窗口', '1024 samples'], ['Overlap', '75%'], ['动态范围', '80 dB']] : [['对象数量', '0'], ['操作', '查看 / 清空']];
  body.innerHTML = `${common}<div class="property-group"><div class="property-group-title"><span>分析参数</span><span class="status-chip primary">模拟</span></div><div class="property-list">${graphInfo.map(([label, value]) => `<div class="property-row"><label>${label}</label><span class="property-value">${value}</span></div>`).join('')}</div></div><div class="property-group"><div class="property-group-title"><span>可见性</span><span class="subtle">当前工作区</span></div><div class="property-list"><div class="toggle-line"><span>显示图谱</span><label class="switch"><input type="checkbox" checked><span class="slider"></span></label></div><div class="toggle-line"><span>参与联动</span><label class="switch"><input type="checkbox" ${appState.linked ? 'checked' : ''} data-action="toggle-link"><span class="slider"></span></label></div></div></div>`;
}

function renderTaskRows(target, full = false) {
  if (!appState.tasks.length) { target.innerHTML = `<div class="empty-bottom"><span>▤</span><div><strong>暂无后台任务</strong><small>导入模拟 IQ 后，任务队列会显示读取、FFT、STFT 和缓存状态。</small></div></div>`; return; }
  target.innerHTML = appState.tasks.map((item) => `<div class="task-row"><span class="task-state status-chip ${stateClass(item.status)}">${taskStatusText[item.status]}</span><span class="task-name"><strong>${item.name}</strong><small>${item.type}</small></span><div class="task-progress"><div class="progress-track"><div class="progress-bar ${stateClass(item.status)}" style="width:${Math.round(item.progress)}%"></div></div><span class="task-percent">${Math.round(item.progress)}%</span></div><span class="task-time">${item.elapsed}</span><span class="task-actions">${taskActions(item, full)}</span></div>`).join('');
}
function taskActions(item, full) {
  if (item.status === 'running') return `<button class="tiny-action" data-task-action="pause" data-task-id="${item.id}">暂停</button><button class="tiny-action" data-task-action="cancel" data-task-id="${item.id}">取消</button>`;
  if (item.status === 'paused') return `<button class="tiny-action" data-task-action="resume" data-task-id="${item.id}">继续</button><button class="tiny-action" data-task-action="cancel" data-task-id="${item.id}">取消</button>`;
  if (['failed', 'cancelled'].includes(item.status)) return `<button class="tiny-action" data-task-action="retry" data-task-id="${item.id}">重试</button><button class="tiny-action" data-task-action="log" data-task-id="${item.id}">日志</button>`;
  if (item.status === 'completed' && full) return `<button class="tiny-action" data-task-action="result" data-task-id="${item.id}">查看结果</button>`;
  return '';
}

function filteredLogs(full = false) {
  const level = document.querySelector(full ? '#full-log-level' : '#log-level')?.value || 'all'; const search = (document.querySelector(full ? '#full-log-search' : '#log-search')?.value || '').toLowerCase();
  return appState.logs.filter((log) => (level === 'all' || log.level === level) && (!search || `${log.message} ${log.taskName}`.toLowerCase().includes(search))).slice(0, full ? 120 : 12);
}
function renderLogs(target, full = false) {
  const logs = filteredLogs(full); target.innerHTML = logs.length ? logs.map((log) => `<div class="log-row"><span class="log-time">${log.time}</span><span class="log-level ${log.level}">${log.level}</span><span class="log-message">${esc(log.message)}</span></div>`).join('') : '<div class="empty-bottom"><span>≋</span><div><strong>暂无匹配日志</strong><small>调整筛选条件后重试。</small></div></div>';
}

function renderBottom() {
  document.querySelector('#bottom-task-badge').textContent = appState.tasks.filter((item) => ['running', 'queued', 'paused', 'retrying'].includes(item.status)).length;
  document.querySelector('#bottom-log-badge').textContent = appState.logs.length;
  renderTaskRows(document.querySelector('#bottom-tasks'));
  renderLogs(document.querySelector('#bottom-logs'));
  document.querySelectorAll('.bottom-tab').forEach((tab) => tab.classList.toggle('active', tab.dataset.bottomTab === appState.bottomTab));
  document.querySelectorAll('.bottom-tab-content').forEach((tab) => tab.classList.toggle('active', tab.id === `bottom-${appState.bottomTab}`));
}

function renderFullTasks() { renderTaskRows(document.querySelector('#full-task-list'), true); renderLogs(document.querySelector('#full-log-list'), true); document.querySelector('#task-count').textContent = appState.tasks.length; document.querySelector('#full-log-count').textContent = appState.logs.length; }

function updateStatus() {
  const source = appState.dataState === 'empty' ? '未打开数据源' : 'sample_100ms_IQ.raw';
  document.querySelector('#toolbar-source').textContent = appState.dataState === 'empty' ? '未打开' : 'sample_100ms_IQ.raw';
  const status = document.querySelector('#toolbar-state'); status.textContent = appState.dataState === 'empty' ? '空状态' : appState.dataState === 'importing' ? '后台加载' : appState.dataState === 'error' ? '读取错误' : '宽带就绪'; status.className = `status-chip ${appState.dataState === 'error' ? 'error' : appState.dataState === 'importing' ? 'warning' : appState.dataState === 'ready' ? 'success' : 'neutral'}`;
  document.querySelector('#status-source').textContent = source; document.querySelector('#status-sample').textContent = appState.dataState === 'empty' ? '采样率 —' : '250 MSps'; document.querySelector('#status-center').textContent = appState.dataState === 'empty' ? '中心频率 —' : '中心 100 MHz';
  const main = document.querySelector('#status-main'); main.innerHTML = `<span class="status-dot ${appState.dataState === 'error' ? 'error' : appState.dataState === 'importing' ? 'warning' : appState.dataState === 'ready' ? 'success' : 'neutral'}"></span>${esc(appState.runtime.status)}`;
  document.querySelector('#status-cache').textContent = appState.dataState === 'empty' ? '0 B' : `${Math.min(95, 16 + appState.tasks.filter((item) => item.status === 'completed').length * 12)} MiB`;
  document.querySelector('#status-time').textContent = appState.cursors.M1 !== null ? `M1 ${(appState.cursors.M1 * 100).toFixed(2)} ms` : '时间 —';
  document.querySelector('#status-cursor').textContent = appState.cursors.C1 !== null ? `C1 ${((appState.cursors.C1 - .5) * 250).toFixed(2)} MHz` : '游标 —';
  document.querySelector('#status-cpu').textContent = `${String(appState.runtime.cpu).padStart(2, '0')}%`; document.querySelector('#status-memory').textContent = `${appState.runtime.memory} MB`;
}

function renderPanels() {
  root.classList.toggle('left-collapsed', appState.panel.left); root.classList.toggle('right-collapsed', appState.panel.right); root.classList.toggle('bottom-collapsed', appState.panel.bottom);
  const mainGrid = document.querySelector('.main-grid'); mainGrid.classList.toggle('left-collapsed', appState.panel.left); mainGrid.classList.toggle('right-collapsed', appState.panel.right);
  document.querySelector('#left-panel').classList.toggle('collapsed', appState.panel.left); document.querySelector('#right-panel').classList.toggle('collapsed', appState.panel.right); document.querySelector('#bottom-panel').classList.toggle('collapsed', appState.panel.bottom);
}

function updateWorkbenchDetails() {
  const badge = document.querySelector('#resolution-badge'); if (!badge) return;
  badge.className = `status-chip ${appState.dataState === 'error' ? 'error' : appState.dataState === 'importing' ? 'warning' : 'success'}`; badge.textContent = appState.dataState === 'error' ? '缓存错误' : appState.dataState === 'importing' ? `低分辨率预览 · ${Math.round((appState.tasks.reduce((a, b) => a + b.progress, 0) / Math.max(1, appState.tasks.length)))}%` : '高分辨率已就绪';
  const error = document.querySelector('#error-banner'); if (error) { error.classList.toggle('hidden', !appState.error); error.innerHTML = appState.error ? `<span class="error-icon">!</span><div><strong>${esc(appState.error.title)} · ${appState.error.code}</strong><span>${esc(appState.error.detail)} ${esc(appState.error.suggestion)}</span></div><div class="error-actions"><button class="secondary-button compact" type="button" data-action="retry-error">重试</button><button class="secondary-button compact" type="button" data-action="open-log">查看日志</button></div>` : ''; }
  const selection = appState.selection; const strip = document.querySelector('#selection-strip'); if (strip) { if (selection) { const start = Math.min(selection.start, selection.end); const end = Math.max(selection.start, selection.end); const isSpectrum = selection.key === 'spectrum'; const a = isSpectrum ? (start - .5) * 250 : start * 100; const b = isSpectrum ? (end - .5) * 250 : end * 100; const unit = isSpectrum ? 'MHz' : 'ms'; strip.innerHTML = `<span class="selection-marker">⌖</span><div><strong>已选择区域 · ${isSpectrum ? '频率' : '时间'}</strong><span>${a.toFixed(2)}–${b.toFixed(2)} ${unit} · 中心 ${((a + b) / 2).toFixed(2)} ${unit} · 带宽 ${Math.abs(b - a).toFixed(2)} ${unit}</span></div><button class="secondary-button compact" type="button" disabled title="M2 功能，当前原型不实现">创建窄带通道 · M2</button>`; } else { strip.innerHTML = '<span class="selection-marker">⌖</span><div><strong>未选择信号区域</strong><span>在图谱上拖拽创建选区；选择后可查看中心频率和带宽。</span></div><button class="secondary-button compact" type="button" disabled title="M2 功能，当前原型不实现">创建窄带通道 · M2</button>'; } }
  document.querySelector('#link-state')?.classList.toggle('success', appState.linked);
}

function renderApp() {
  document.querySelector('#welcome-view').classList.toggle('hidden', appState.view !== 'welcome'); document.querySelector('#workbench-view').classList.toggle('hidden', appState.view !== 'workbench'); document.querySelector('#tasks-view').classList.toggle('hidden', appState.view !== 'tasks');
  renderTree(); renderInspector(); renderBottom(); renderPanels(); updateStatus(); updateWorkbenchDetails();
  if (appState.view === 'workbench') window.requestAnimationFrame(() => charts?.drawAll());
  if (appState.view === 'tasks') renderFullTasks();
}

function handleImportConfirm(config) {
  appState.importConfig = { ...config }; appState.importOpen = false; document.querySelector('#wizard-overlay').classList.add('hidden'); appState.selectedNode = 'file'; resetCharts(); taskQueue.resetImportTasks(); router.openWorkbench(); showToast('已确认导入', 'success', '低分辨率宽带预览正在生成。');
}

function handleTreeSelect(id) {
  const node = nodeTree.find((item) => item.id === id); if (!node || node.disabled) { showToast('后续版本入口', 'warning', '调制分析属于 M2，当前仅保留禁用入口。'); return; } appState.selectedNode = id; appState.dirty = false; propertyDraft = {}; renderApp();
}

function handleTaskAction(action, id) {
  if (action === 'pause') taskQueue.pause(); else if (action === 'resume') taskQueue.resume(); else if (action === 'cancel') taskQueue.cancel(); else if (action === 'retry') taskQueue.retry(id); else if (action === 'log') { appState.bottomTab = 'logs'; renderApp(); } else if (action === 'result') showToast('结果面板', 'info', '该任务的结果已由当前工作区展示。');
}

function handlePropertyInput(event) { const field = event.target.dataset.propertyField; if (!field) return; propertyDraft[field] = event.target.value; appState.dirty = true; event.target.closest('.property-group')?.querySelector('.dirty-note')?.classList.remove('hidden'); }
function applyProperties() { appState.dirty = false; addLog('info', `已应用属性修改：${propertyDraft.fileName || '当前对象'}。`, '属性编辑'); renderApp(); showToast('属性已应用', 'success'); }
function resetProperties() { appState.dirty = false; propertyDraft = {}; renderApp(); showToast('属性已重置', 'info'); }

function selectBottom(tab) { appState.bottomTab = tab; renderBottom(); }

function openMenu(button) { const pop = document.querySelector('#menu-popover'); const menu = button.dataset.menu; const items = { 文件: ['新建项目', '打开 IQ 文件', '保存项目'], 查看: ['适配视图', '重置图谱', '折叠面板'], 分析: ['宽带浏览', '创建窄带通道（M2）', '测量'], 工具: ['模拟读取错误', '查看日志', '使用提示'], 窗口: ['任务与日志', '项目属性'], 帮助: ['原型说明', '验收范围'] }; pop.innerHTML = (items[menu] || []).map((item) => `<button type="button" data-menu-command="${esc(item)}">${esc(item)}</button>`).join(''); pop.classList.remove('hidden'); menuOpen = true; }

function handleMenuCommand(command) { document.querySelector('#menu-popover').classList.add('hidden'); menuOpen = false; if (command.includes('IQ')) wizard.open(); else if (command.includes('任务')) router.openTasks(); else if (command.includes('错误')) taskQueue.failRead(); else if (command.includes('适配') || command.includes('重置')) { charts?.reset(); showToast('视图已重置', 'info'); } else showToast(command, 'info', '该菜单命令在原型中以可视化状态演示。'); }

document.addEventListener('click', (event) => {
  const actionTarget = event.target.closest('[data-action]'); if (actionTarget) {
    const action = actionTarget.dataset.action;
    if (action === 'open-iq') wizard.open(); else if (action === 'close-wizard' || action === 'cancel-wizard') wizard.close(); else if (action === 'wizard-next' || action === 'confirm-import') wizard.nextStep(); else if (action === 'wizard-back') wizard.backStep(); else if (action === 'choose-file') { appState.importConfig.path = 'D:/data/sample_100ms_IQ.raw'; wizard.render(); showToast('已选择模拟文件', 'info'); } else if (action === 'new-project') { appState.dataState = 'empty'; appState.selectedNode = null; appState.tasks = []; appState.logs = []; appState.error = null; resetCharts(); router.openWelcome(); showToast('已新建空项目', 'success'); } else if (action === 'open-project') showToast('项目打开入口', 'info', 'M1 原型只演示 RAW IQ 导入流程。'); else if (action === 'save-project') { appState.dirty = false; addLog('info', '项目配置已保存（模拟）。', '项目'); renderApp(); showToast('项目已保存', 'success'); } else if (action === 'fit-view') { charts?.reset(); showToast('图谱已适配', 'info'); } else if (action === 'measure') { appState.graphMode = 'double-cursor'; document.querySelectorAll('[data-mode]').forEach((b) => b.classList.toggle('active', b.dataset.mode === 'double-cursor')); showToast('双游标模式', 'info', '点击时域或频谱图放置游标。'); } else if (action === 'add-mark') { appState.marks.push({ time: '52.40 ms', label: `标记 ${appState.marks.length + 1}` }); addLog('info', `已添加标记：${appState.marks.at(-1).label}。`, '标记'); selectBottom('marks'); renderApp(); showToast('已添加标记', 'success'); } else if (action === 'capture') showToast('截图已准备', 'info', '原型不会写入系统剪贴板或文件。'); else if (action === 'simulate-error') taskQueue.failRead(); else if (action === 'retry-error') taskQueue.retry('read'); else if (action === 'open-log') { appState.bottomTab = 'logs'; renderApp(); } else if (action === 'return-workbench') router.openWorkbench(); else if (action === 'create-channel') showToast('M2 功能未纳入本轮', 'warning'); else if (action === 'toggle-link') { appState.linked = !appState.linked; renderApp(); showToast(appState.linked ? '图谱联动已开启' : '图谱联动已关闭', 'info'); } else if (action === 'toggle-theme') { appState.themeAlt = !appState.themeAlt; root.classList.toggle('theme-alt', appState.themeAlt); showToast('强调色已切换', 'info'); } else if (action === 'show-help') document.querySelector('#help-popover').classList.toggle('hidden'); else if (action === 'close-help') document.querySelector('#help-popover').classList.add('hidden'); else if (action === 'apply-properties') applyProperties(); else if (action === 'reset-properties') resetProperties(); else if (action === 'clear-logs') { taskQueue.clearLogs(); renderApp(); } else if (action === 'collapse-left' || action === 'collapse-right') {} }
  const menuButton = event.target.closest('.menu-item'); if (menuButton) { if (menuOpen && document.querySelector('#menu-popover').contains(event.target)) return; openMenu(menuButton); return; }
  const menuCommand = event.target.closest('[data-menu-command]'); if (menuCommand) { handleMenuCommand(menuCommand.dataset.menuCommand); return; }
  const collapse = event.target.closest('[data-collapse]'); if (collapse) { const key = collapse.dataset.collapse; appState.panel[key] = !appState.panel[key]; renderPanels(); return; }
  const node = event.target.closest('[data-node-id]'); if (node) { handleTreeSelect(node.dataset.nodeId); return; }
  const mode = event.target.closest('[data-mode]'); if (mode) { appState.graphMode = mode.dataset.mode; document.querySelectorAll('[data-mode]').forEach((button) => button.classList.toggle('active', button === mode)); return; }
  const bottom = event.target.closest('[data-bottom-tab]'); if (bottom) { selectBottom(bottom.dataset.bottomTab); return; }
  const taskAction = event.target.closest('[data-task-action]'); if (taskAction) { handleTaskAction(taskAction.dataset.taskAction, taskAction.dataset.taskId); return; }
  if (menuOpen && !event.target.closest('#menu-popover')) { document.querySelector('#menu-popover').classList.add('hidden'); menuOpen = false; }
});

document.addEventListener('input', (event) => { handlePropertyInput(event); if (event.target.id === 'full-log-search' || event.target.id === 'log-search') { renderLogs(document.querySelector(event.target.id === 'full-log-search' ? '#full-log-list' : '#bottom-logs'), Boolean(event.target.id === 'full-log-search')); } });
document.addEventListener('change', (event) => { if (event.target.id === 'full-log-level' || event.target.id === 'log-level') { renderLogs(document.querySelector(event.target.id === 'full-log-level' ? '#full-log-list' : '#bottom-logs'), Boolean(event.target.id === 'full-log-level')); } });

taskQueue.subscribe(() => { renderBottom(); updateStatus(); updateWorkbenchDetails(); if (appState.view === 'tasks') renderFullTasks(); if (appState.dataState === 'ready') { document.querySelector('#resolution-badge')?.classList.add('success'); charts?.drawAll(); } });

charts = createCharts({ onInteraction: ({ key, type }) => { if (appState.linked && key !== 'stft') { const peer = key === 'time' ? 'stft' : 'time'; appState.chart[peer].zoom = appState.chart[key].zoom; appState.chart[peer].offset = appState.chart[key].offset; } if (type === 'selection') { updateWorkbenchDetails(); addLog('info', `已在${key === 'spectrum' ? '频谱' : key === 'stft' ? '时频图' : '时域'}创建选区。`, '图谱交互'); } updateStatus(); }, onReadout: (key, u) => { const overlay = document.querySelector(`#${key === 'time' ? 'time' : key === 'spectrum' ? 'spectrum' : 'stft'}-overlay`); if (overlay) overlay.textContent = `${key === 'spectrum' ? '频率' : '时间'} ${getGraphReadout(key, u)}`; } });

window.setInterval(() => { appState.runtime.cpu = 6 + Math.floor(Math.random() * 8); appState.runtime.memory = 408 + Math.floor(Math.random() * 20); updateStatus(); }, 2200);

renderApp();
