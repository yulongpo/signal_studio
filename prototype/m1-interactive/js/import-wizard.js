import { appState, signalConfig, formatNumber } from './state.js';

const steps = ['文件与参数', '数据预览', '参数校验', '完成'];

function esc(value) {
  return String(value ?? '').replace(/[&<>"']/g, (char) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#039;' }[char]));
}

function inputRow(label, name, value, options = {}) {
  const { unit, type = 'text', full = false, hint = '', required = false, min, step } = options;
  const control = unit ? `<div class="input-with-addon"><input class="form-control" data-import-field="${name}" name="${name}" type="${type}" value="${esc(value)}" ${min !== undefined ? `min="${min}"` : ''} ${step !== undefined ? `step="${step}"` : ''}><select class="form-control" data-import-unit="${name}"><option>${esc(unit)}</option><option>${name === 'sampleRate' ? 'MSps' : 'MHz'}</option></select></div>` : `<input class="form-control" data-import-field="${name}" name="${name}" type="${type}" value="${esc(value)}" ${min !== undefined ? `min="${min}"` : ''} ${step !== undefined ? `step="${step}"` : ''}>`;
  return `<div class="form-row ${full ? 'full' : ''}"><label for="import-${name}"><span>${label} ${required ? '<em>*</em>' : ''}</span></label>${control}<div class="field-error" data-error-for="${name}"></div><div class="field-hint">${hint}</div></div>`;
}

function selectRow(label, name, value, choices, hint = '') {
  return `<div class="form-row"><label for="import-${name}"><span>${label}</span></label><select id="import-${name}" class="form-control" data-import-field="${name}" name="${name}">${choices.map((choice) => `<option ${choice === value ? 'selected' : ''}>${choice}</option>`).join('')}</select><div class="field-error" data-error-for="${name}"></div><div class="field-hint">${hint}</div></div>`;
}

function renderProgress() {
  return steps.map((label, index) => {
    const step = index + 1; const state = step < appState.importStep ? 'done' : step === appState.importStep ? 'active' : '';
    return `<div class="step-item ${state}"><span class="step-number">${step < appState.importStep ? '✓' : step}</span><span>${label}</span></div>`;
  }).join('');
}

function renderStep1() {
  const cfg = appState.importConfig;
  return `<div class="wizard-section-title"><h3>文件与采集参数</h3><p>选择 RAW IQ 的字节解释方式。当前只演示配置、预览和校验，不会真正读取本地路径。</p></div>
    <div class="form-grid">
      <div class="form-row full"><label for="import-path"><span>文件路径 <em>*</em></span><span class="subtle">模拟文件</span></label><div class="file-input-row"><input id="import-path" class="form-control" data-import-field="path" value="${esc(cfg.path)}" placeholder="例如 D:/data/capture.raw"><button class="secondary-button compact" type="button" data-action="choose-file">浏览…</button></div><div class="field-error" data-error-for="path"></div><div class="field-hint">模拟路径：sample_100ms_IQ.raw · 95.37 MiB</div></div>
      ${selectRow('数据类型', 'dataType', cfg.dataType, ['Int8', 'Int16', 'Float32'], '每个 I/Q 分量的数据类型')}
      ${selectRow('IQ 顺序', 'iqOrder', cfg.iqOrder, ['IQ', 'QI'], '交错存储：I0 Q0 I1 Q1 …')}
      ${selectRow('端序', 'endian', cfg.endian, ['Little Endian', 'Big Endian'], '仅适用于 Int16 / Float32')}
      ${inputRow('采样率', 'sampleRate', cfg.sampleRate, { unit: cfg.sampleRateUnit, type: 'number', required: true, min: 0, step: 1, hint: 'M1 模拟配置：250 MSps' })}
      ${inputRow('中心频率', 'centerFrequency', cfg.centerFrequency, { unit: cfg.centerFrequencyUnit, type: 'number', required: true, min: 0, step: 1, hint: '用于频谱坐标换算' })}
      ${inputRow('Header 偏移', 'headerOffset', cfg.headerOffset, { unit: cfg.headerUnit, type: 'number', required: true, min: 0, step: 1, hint: '文件头长度，默认 0 bytes' })}
      ${inputRow('振幅缩放', 'amplitudeScale', cfg.amplitudeScale, { type: 'number', required: true, min: 0, step: .01, hint: '显示振幅的模拟缩放因子' })}
      ${inputRow('起始 UTC 时间', 'startUtc', cfg.startUtc, { type: 'datetime-local', full: false, hint: '用于时间轴绝对时间显示' })}
    </div>
    <div class="wizard-callout"><span class="status-dot primary"></span><div><strong>低分辨率优先：</strong>导入确认后会先展示低分辨率概览，后台模拟生成时域概要、FFT 和 STFT 结果。所有计算均为确定性模拟。</div></div>`;
}

function renderPreview() {
  return `<div class="wizard-section-title"><h3>数据预览</h3><p>预览前 8 个复采样点和文件元信息，帮助确认端序、IQ 顺序与振幅范围。</p></div>
    <div class="preview-layout"><div class="preview-card"><h4>复采样点 / simulated preview</h4><table class="preview-table"><thead><tr><th>#</th><th>I</th><th>Q</th><th>幅度</th></tr></thead><tbody>${[0,1,2,3,4,5].map((i) => `<tr><td>${i}</td><td>${(0.21 + Math.sin(i * .8) * .16).toFixed(4)}</td><td>${(-0.08 + Math.cos(i * .9) * .18).toFixed(4)}</td><td>${(.24 + i * .018).toFixed(4)}</td></tr>`).join('')}</tbody></table><canvas id="preview-canvas" class="mini-chart" aria-label="预览波形"></canvas></div><div class="preview-card"><h4>文件摘要</h4><div class="preview-stat"><span>文件大小</span><strong>95.37 MiB</strong></div><div class="preview-stat"><span>复采样点数</span><strong>25,000,000</strong></div><div class="preview-stat"><span>时长</span><strong>100.000 ms</strong></div><div class="preview-stat"><span>数据率</span><strong>500.00 MB/s</strong></div><div class="preview-stat"><span>解释方式</span><strong>${esc(appState.importConfig.dataType)} · ${esc(appState.importConfig.iqOrder)}</strong></div><div class="preview-stat"><span>端序</span><strong>${esc(appState.importConfig.endian)}</strong></div><div class="preview-stat"><span>Header</span><strong>${esc(appState.importConfig.headerOffset)} bytes</strong></div></div></div>
    <div class="wizard-callout"><span class="status-dot warning"></span><div><strong>提示：</strong>这是本地模拟预览；实际产品需要在后台线程分块读取并避免将大文件完整载入内存。</div></div>`;
}

function renderValidation() {
  const validation = appState.importValidation || { pass: true, warnings: [] };
  const rows = [
    { ok: validation.pass, label: '必填项与数值范围', detail: validation.pass ? '文件路径、采样率、中心频率和缩放因子均有效。' : '存在字段错误，请返回上一步修正。' },
    { ok: validation.pass, label: '采样点完整性', detail: '文件大小可被 Int16 IQ 复采样点整除（模拟检查通过）。' },
    { ok: validation.pass, label: '字节解释方式', detail: `${appState.importConfig.dataType} · ${appState.importConfig.iqOrder} · ${appState.importConfig.endian}` },
    { ok: validation.warnings.length === 0, label: '显示范围提示', detail: validation.warnings.length ? validation.warnings.join('；') : '采样率与中心频率可用于 M1 宽带坐标。' }
  ];
  return `<div class="wizard-section-title"><h3>参数校验</h3><p>确认通过后才能开始模拟导入。错误会关联到具体表单字段，警告不阻断导入。</p></div><div class="validation-card"><div class="validation-list">${rows.map((row) => `<div class="validation-item ${row.ok ? 'pass' : 'fail'}"><span class="validation-icon">${row.ok ? '✓' : '!'}</span><div><strong>${row.label}</strong><span>${row.detail}</span></div></div>`).join('')}</div></div>${validation.warnings.length ? '<div class="wizard-callout"><span class="status-dot warning"></span><div><strong>存在警告：</strong>建议确认显示参数，但仍可继续。</div></div>' : '<div class="wizard-callout"><span class="status-dot success"></span><div><strong>校验通过：</strong>可以确认导入并开始后台任务。</div></div>'}`;
}

function renderComplete() {
  const cfg = appState.importConfig;
  return `<div class="complete-card"><div class="complete-icon">✓</div><div><h4>配置已就绪</h4><p>点击“确认导入”后将创建模拟任务队列，并进入低分辨率宽带浏览工作区。</p></div></div><div class="wizard-summary"><div class="wizard-summary-row"><span>文件</span><strong>${esc(cfg.path.split('/').pop().split('\\').pop())}</strong></div><div class="wizard-summary-row"><span>格式</span><strong>${esc(cfg.dataType)} · ${esc(cfg.iqOrder)} · ${esc(cfg.endian)}</strong></div><div class="wizard-summary-row"><span>采样率 / 中心</span><strong>250 MSps / 100 MHz</strong></div><div class="wizard-summary-row"><span>模拟数据</span><strong>25,000,000 complex · 100 ms</strong></div></div><div class="wizard-callout"><span class="status-dot success"></span><div><strong>导入范围：</strong>RAW IQ → 低分辨率预览 → 时域 / 频谱 / STFT；窄带、调制识别、星座图和眼图属于 M2/M3。</div></div>`;
}

function drawPreview() {
  const canvas = document.querySelector('#preview-canvas'); if (!canvas) return;
  const rect = canvas.getBoundingClientRect(); const dpr = Math.min(window.devicePixelRatio || 1, 2); canvas.width = rect.width * dpr; canvas.height = rect.height * dpr; const ctx = canvas.getContext('2d'); ctx.scale(dpr, dpr); const w = rect.width; const h = rect.height;
  ctx.fillStyle = '#0c1422'; ctx.fillRect(0, 0, w, h); ctx.strokeStyle = 'rgba(142,162,187,.14)'; ctx.beginPath(); for (let i = 1; i < 4; i += 1) { ctx.moveTo(0, h * i / 4); ctx.lineTo(w, h * i / 4); } ctx.stroke();
  for (const [color, phase] of [['#ff9f5b', 0], ['#3ebdff', 1.3]]) { ctx.strokeStyle = color; ctx.lineWidth = 1.3; ctx.beginPath(); for (let i = 0; i < w; i += 1) { const y = h * (.52 + Math.sin(i * .13 + phase) * .22 + Math.sin(i * .39) * .04); if (!i) ctx.moveTo(i, y); else ctx.lineTo(i, y); } ctx.stroke(); }
}

export function validateConfig(config) {
  const errors = {}; const warnings = [];
  if (!String(config.path || '').trim()) errors.path = '请输入文件路径。';
  if (!Number.isFinite(Number(config.sampleRate)) || Number(config.sampleRate) <= 0) errors.sampleRate = '采样率必须大于 0。';
  if (!Number.isFinite(Number(config.centerFrequency)) || Number(config.centerFrequency) < 0) errors.centerFrequency = '中心频率必须是有效的非负数。';
  if (!Number.isFinite(Number(config.headerOffset)) || Number(config.headerOffset) < 0) errors.headerOffset = 'Header 偏移不能为负数。';
  if (!Number.isFinite(Number(config.amplitudeScale)) || Number(config.amplitudeScale) <= 0) errors.amplitudeScale = '振幅缩放必须大于 0。';
  if (Number(config.headerOffset) > 1024 * 1024) warnings.push('Header 偏移较大，请确认文件格式。');
  return { pass: Object.keys(errors).length === 0, errors, warnings };
}

export function createWizard({ onChange, onFinish, onClose }) {
  const overlay = document.querySelector('#wizard-overlay'); const content = document.querySelector('#wizard-content'); const progress = document.querySelector('#wizard-progress'); const back = document.querySelector('#wizard-back'); const next = document.querySelector('#wizard-next');
  const render = () => {
    progress.innerHTML = renderProgress();
    content.innerHTML = appState.importStep === 1 ? renderStep1() : appState.importStep === 2 ? renderPreview() : appState.importStep === 3 ? renderValidation() : renderComplete();
    back.disabled = appState.importStep === 1;
    next.textContent = appState.importStep === 3 ? '重新校验' : appState.importStep === 4 ? '确认导入' : '下一步';
    next.dataset.action = appState.importStep === 4 ? 'confirm-import' : 'wizard-next';
    bindFields(); drawPreview();
  };
  const bindFields = () => content.querySelectorAll('[data-import-field]').forEach((input) => input.addEventListener('input', () => { appState.importConfig[input.name] = input.value; input.classList.remove('error'); const error = content.querySelector(`[data-error-for="${input.name}"]`); if (error) error.textContent = ''; onChange?.(); }));
  const showErrors = (validation) => { for (const [name, message] of Object.entries(validation.errors)) { const input = content.querySelector(`[data-import-field="${name}"]`); const error = content.querySelector(`[data-error-for="${name}"]`); input?.classList.add('error'); if (error) error.textContent = message; } };
  const open = () => { appState.importOpen = true; appState.importStep = 1; appState.importConfig = { ...signalConfig }; appState.importValidation = null; overlay.classList.remove('hidden'); render(); content.querySelector('[data-import-field="path"]')?.focus(); };
  const close = () => { appState.importOpen = false; overlay.classList.add('hidden'); onClose?.(); };
  const nextStep = () => {
    if (appState.importStep === 1) { const validation = validateConfig(appState.importConfig); appState.importValidation = validation; if (!validation.pass) { showErrors(validation); onChange?.('参数校验失败'); return; } }
    if (appState.importStep === 3) { const validation = validateConfig(appState.importConfig); appState.importValidation = validation; if (!validation.pass) { appState.importStep = 1; render(); showErrors(validation); return; } }
    if (appState.importStep < 4) { appState.importStep += 1; render(); }
    else onFinish?.({ ...appState.importConfig });
  };
  const backStep = () => { if (appState.importStep > 1) { appState.importStep -= 1; render(); } };
  overlay.addEventListener('click', (event) => { if (event.target === overlay) close(); });
  return { open, close, render, nextStep, backStep };
}
