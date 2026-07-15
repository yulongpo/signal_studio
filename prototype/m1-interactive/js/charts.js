import { appState, formatNumber } from './state.js';

const DPR = () => Math.min(window.devicePixelRatio || 1, 2);
const COLORS = { grid: 'rgba(142,162,187,.12)', axis: '#667994', text: '#8ea2bb', i: '#ff9f5b', q: '#3ebdff', green: '#37d7a0', selection: 'rgba(40,169,255,.16)', cursor: '#f7b84b' };

function setupCanvas(canvas) {
  if (!canvas) return null;
  const rect = canvas.getBoundingClientRect();
  const width = Math.max(160, Math.floor(rect.width));
  const height = Math.max(90, Math.floor(rect.height));
  const ratio = DPR();
  if (canvas.width !== width * ratio || canvas.height !== height * ratio) {
    canvas.width = width * ratio;
    canvas.height = height * ratio;
  }
  const ctx = canvas.getContext('2d');
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
  return { ctx, width, height };
}

function clear(ctx, width, height) {
  ctx.fillStyle = '#0c1422';
  ctx.fillRect(0, 0, width, height);
}

function drawGrid(ctx, width, height, rows = 4, cols = 8) {
  ctx.strokeStyle = COLORS.grid;
  ctx.lineWidth = 1;
  ctx.beginPath();
  for (let i = 1; i < rows; i += 1) { const y = Math.round(height * i / rows) + .5; ctx.moveTo(0, y); ctx.lineTo(width, y); }
  for (let i = 1; i < cols; i += 1) { const x = Math.round(width * i / cols) + .5; ctx.moveTo(x, 0); ctx.lineTo(x, height); }
  ctx.stroke();
}

function drawAxes(ctx, width, height, labels, yLabels = ['0', '−20', '−40', '−60']) {
  ctx.fillStyle = COLORS.text;
  ctx.font = '9px Consolas, monospace';
  ctx.textAlign = 'center';
  labels.forEach((label, index) => ctx.fillText(label, width * index / (labels.length - 1), height - 5));
  ctx.textAlign = 'left';
  yLabels.forEach((label, index) => ctx.fillText(label, 5, height * index / (yLabels.length - 1) + 10));
}

function visibleX(local, zoom, offset) {
  return (local - .5) * zoom + .5 + offset;
}

function drawSelectionAndCursors(ctx, width, height, chartKey) {
  const chart = appState.chart[chartKey];
  const selection = chart.selection;
  if (selection) {
    const left = Math.min(selection.start, selection.end) * width;
    const right = Math.max(selection.start, selection.end) * width;
    ctx.fillStyle = COLORS.selection;
    ctx.fillRect(left, 0, Math.max(1, right - left), height);
    ctx.strokeStyle = COLORS.primary || '#28a9ff';
    ctx.setLineDash([4, 3]);
    ctx.strokeRect(left + .5, .5, Math.max(1, right - left - 1), height - 1);
    ctx.setLineDash([]);
  }
  const cursors = chartKey === 'spectrum' ? ['C1'] : ['M1', 'M2'];
  cursors.forEach((key) => {
    const position = appState.cursors[key];
    if (position === null || position === undefined) return;
    const x = position * width;
    ctx.strokeStyle = COLORS.cursor;
    ctx.lineWidth = 1;
    ctx.setLineDash([5, 3]);
    ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, height); ctx.stroke(); ctx.setLineDash([]);
    ctx.fillStyle = COLORS.cursor;
    ctx.font = '9px Consolas, monospace';
    ctx.fillText(key, Math.min(width - 22, x + 4), 14);
  });
}

function drawTime(canvas) {
  const result = setupCanvas(canvas); if (!result) return;
  const { ctx, width, height } = result;
  clear(ctx, width, height); drawGrid(ctx, width, height); drawAxes(ctx, width, height, ['0', '25', '50', '75', '100'], ['0', '−20', '−40', '−60']);
  const chart = appState.chart.time;
  const samples = Math.max(280, Math.floor(width * 1.2));
  for (const channel of ['i', 'q']) {
    ctx.beginPath(); ctx.lineWidth = 1.25; ctx.strokeStyle = channel === 'i' ? COLORS.i : COLORS.q;
    for (let n = 0; n < samples; n += 1) {
      const u = n / (samples - 1); const x = u * width; const t = u * 16;
      const carrier = channel === 'i' ? Math.sin(t * 7.2) : Math.cos(t * 7.2);
      const burst = Math.exp(-Math.pow((u - .29) / .1, 2)) * Math.sin(t * 17);
      const drift = Math.sin(t * 1.1) * .13 + Math.sin(t * 29) * .03;
      const amp = channel === 'i' ? carrier * .38 + burst * .23 + drift : carrier * .34 - burst * .16 + drift * .8;
      const y = height * (.51 - amp * .74);
      if (n === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    }
    ctx.stroke();
  }
  drawSelectionAndCursors(ctx, width, height, 'time');
  if (chart.zoom > 1.01) { ctx.fillStyle = COLORS.text; ctx.font = '9px Consolas, monospace'; ctx.fillText(`zoom ${chart.zoom.toFixed(1)}×`, width - 60, 14); }
}

function drawSpectrum(canvas) {
  const result = setupCanvas(canvas); if (!result) return;
  const { ctx, width, height } = result;
  clear(ctx, width, height); drawGrid(ctx, width, height); drawAxes(ctx, width, height, ['−125', '−62.5', '0', '62.5', '125'], ['0', '−20', '−40', '−60']);
  const chart = appState.chart.spectrum;
  const points = Math.max(340, width);
  for (const average of [true, false]) {
    ctx.beginPath(); ctx.lineWidth = average ? 1 : 1.35; ctx.strokeStyle = average ? 'rgba(142,162,187,.7)' : COLORS.green;
    if (average) ctx.setLineDash([3, 3]);
    for (let n = 0; n < points; n += 1) {
      const u = n / (points - 1); const visible = visibleX(u, chart.zoom, chart.offset); const x = u * width; const freq = (visible - .5) * 250;
      const noise = -58 + Math.sin(freq * .55) * 2.2 + Math.sin(freq * 2.8) * 1.2;
      const peakA = 42 * Math.exp(-Math.pow((freq + 48) / 6, 2));
      const peakB = 30 * Math.exp(-Math.pow((freq - 3) / 10, 2));
      const peakC = 35 * Math.exp(-Math.pow((freq - 58) / 4, 2));
      const db = noise + peakA + peakB + peakC + (average ? -3 : 0);
      const y = height * (.12 + ((-db) - 0) / 80 * .82);
      if (n === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    }
    ctx.stroke(); ctx.setLineDash([]);
  }
  drawSelectionAndCursors(ctx, width, height, 'spectrum');
  if (chart.zoom > 1.01) { ctx.fillStyle = COLORS.text; ctx.font = '9px Consolas, monospace'; ctx.fillText(`zoom ${chart.zoom.toFixed(1)}×`, width - 60, 14); }
}

function inferno(t) {
  const stops = [[18, 13, 43], [66, 17, 77], [142, 34, 86], [215, 61, 66], [255, 145, 40], [255, 236, 115]];
  const scaled = Math.max(0, Math.min(.999, t)) * (stops.length - 1); const i = Math.floor(scaled); const f = scaled - i; const a = stops[i]; const b = stops[Math.min(stops.length - 1, i + 1)];
  return `rgb(${Math.round(a[0] + (b[0] - a[0]) * f)},${Math.round(a[1] + (b[1] - a[1]) * f)},${Math.round(a[2] + (b[2] - a[2]) * f)})`;
}

function drawStft(canvas) {
  const result = setupCanvas(canvas); if (!result) return;
  const { ctx, width, height } = result; clear(ctx, width, height); drawGrid(ctx, width, height, 5, 10);
  const chart = appState.chart.stft; const cols = Math.max(80, Math.floor(width / 5)); const rows = 28; const cellW = width / cols; const cellH = height / rows;
  for (let col = 0; col < cols; col += 1) {
    const u = col / (cols - 1); const visible = visibleX(u, chart.zoom, chart.offset); const time = visible * 100;
    for (let row = 0; row < rows; row += 1) {
      const freq = ((row / (rows - 1)) - .5) * 250;
      const background = .12 + .08 * Math.sin(col * .23) + .05 * Math.cos(row * .8);
      const band1 = .85 * Math.exp(-Math.pow((freq + 45) / 18, 2)) * (.25 + .75 * Math.pow(Math.sin(time * .08), 2));
      const band2 = .75 * Math.exp(-Math.pow((freq - 2) / 22, 2)) * (.2 + .8 * Math.pow(Math.sin(time * .16 + .5), 2));
      const burst = .95 * Math.exp(-Math.pow((time - 70) / 8, 2)) * Math.exp(-Math.pow((freq - 55) / 12, 2));
      const value = Math.max(0, Math.min(1, background + band1 + band2 + burst));
      ctx.fillStyle = inferno(value); ctx.fillRect(col * cellW, height - (row + 1) * cellH, Math.ceil(cellW + .7), Math.ceil(cellH + .7));
    }
  }
  ctx.strokeStyle = 'rgba(255,255,255,.18)'; ctx.strokeRect(.5, .5, width - 1, height - 1);
  drawAxes(ctx, width, height, ['0', '25', '50', '75', '100'], ['125', '62.5', '0', '−62.5', '−125']);
  drawSelectionAndCursors(ctx, width, height, 'stft');
}

function canvasPoint(canvas, event) {
  const rect = canvas.getBoundingClientRect();
  return { x: Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width)), y: Math.max(0, Math.min(1, (event.clientY - rect.top) / rect.height)) };
}

export function getGraphReadout(key, u) {
  if (key === 'spectrum') return `${((u - .5) * 250).toFixed(2)} MHz`;
  return `${(u * 100).toFixed(2)} ms`;
}

export function createCharts({ onInteraction, onReadout }) {
  const canvases = { time: document.querySelector('#time-canvas'), spectrum: document.querySelector('#spectrum-canvas'), stft: document.querySelector('#stft-canvas') };
  let drag = null;
  const drawAll = () => { drawTime(canvases.time); drawSpectrum(canvases.spectrum); drawStft(canvases.stft); };
  const drawOne = (key) => ({ time: drawTime, spectrum: drawSpectrum, stft: drawStft }[key])(canvases[key]);
  const handlePointerDown = (key, event) => {
    if (event.button !== 0) return;
    const point = canvasPoint(canvases[key], event); drag = { key, start: point, current: point, mode: appState.graphMode };
    canvases[key].parentElement.classList.add('dragging'); canvases[key].setPointerCapture?.(event.pointerId);
    if (appState.graphMode === 'cursor' || appState.graphMode === 'double-cursor') {
      if (key === 'spectrum') appState.cursors.C1 = point.x;
      else if (appState.graphMode === 'double-cursor') appState.cursors.M2 = appState.cursors.M1 === null ? point.x : point.x;
      else appState.cursors.M1 = point.x;
      onInteraction({ key, type: 'cursor', point }); drawAll(); return;
    }
    if (appState.graphMode === 'select' && key === 'spectrum') { appState.selection = { start: point.x, end: point.x, key }; onInteraction({ key, type: 'selection', point }); drawAll(); }
  };
  const handlePointerMove = (key, event) => {
    const point = canvasPoint(canvases[key], event); onReadout?.(key, point.x);
    if (!drag || drag.key !== key) return;
    drag.current = point;
    if (drag.mode === 'pan') {
      const delta = point.x - drag.start.x; appState.chart[key].offset = Math.max(-.65, Math.min(.65, appState.chart[key].offset + delta)); drag.start = point; drawOne(key); onInteraction({ key, type: 'pan', point });
    } else if (drag.mode === 'zoom') {
      appState.chart[key].selection = { start: drag.start.x, end: point.x }; drawOne(key); onInteraction({ key, type: 'zoom-box', point });
    } else if (drag.mode === 'range' || (drag.mode === 'select' && key !== 'spectrum')) {
      appState.chart[key].selection = { start: drag.start.x, end: point.x }; appState.selection = { start: drag.start.x, end: point.x, key }; drawOne(key); onInteraction({ key, type: 'selection', point });
    }
  };
  const handlePointerUp = (key, event) => {
    if (!drag || drag.key !== key) return;
    const point = canvasPoint(canvases[key], event); const mode = drag.mode;
    if (mode === 'zoom' && Math.abs(point.x - drag.start.x) > .035) { const span = Math.abs(point.x - drag.start.x); appState.chart[key].zoom = Math.min(8, Math.max(1, appState.chart[key].zoom / span)); appState.chart[key].offset = 0; appState.chart[key].selection = null; onInteraction({ key, type: 'zoom', point }); }
    if (mode === 'select' && key === 'spectrum') { appState.selection = { start: drag.start.x, end: point.x, key }; appState.chart[key].selection = appState.selection; onInteraction({ key, type: 'selection', point }); }
    if (mode === 'range') { appState.selection = { start: drag.start.x, end: point.x, key }; onInteraction({ key, type: 'selection', point }); }
    drawAll(); canvases[key].parentElement.classList.remove('dragging'); canvases[key].releasePointerCapture?.(event.pointerId); drag = null;
  };
  const handleWheel = (key, event) => {
    event.preventDefault(); const chart = appState.chart[key]; const direction = event.deltaY > 0 ? -1 : 1; chart.zoom = Math.min(8, Math.max(1, chart.zoom * (direction > 0 ? 1.16 : .86))); onInteraction({ key, type: 'wheel-zoom', point: canvasPoint(canvases[key], event) }); drawOne(key);
  };
  for (const [key, canvas] of Object.entries(canvases)) {
    canvas.addEventListener('pointerdown', (e) => handlePointerDown(key, e)); canvas.addEventListener('pointermove', (e) => handlePointerMove(key, e)); canvas.addEventListener('pointerup', (e) => handlePointerUp(key, e)); canvas.addEventListener('pointercancel', (e) => handlePointerUp(key, e)); canvas.addEventListener('wheel', (e) => handleWheel(key, e), { passive: false });
  }
  window.addEventListener('resize', drawAll);
  return { drawAll, reset() { for (const chart of Object.values(appState.chart)) { chart.zoom = 1; chart.offset = 0; chart.selection = null; } drawAll(); } };
}
