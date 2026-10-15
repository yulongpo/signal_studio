// Browser-only interaction prototype. The plots and channel plans are synthetic.
import * as Model from './model.mjs';

const $ = selector => document.querySelector(selector);
const $$ = selector => [...document.querySelectorAll(selector)];
const clone = value => structuredClone(value);
const escapeHtml = value =>
  String(value ?? '').replace(
    /[&<>"']/g,
    char =>
      ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[
        char
      ]
  );
const number = (value, digits = 3) => Number(value).toFixed(digits);
const sourceNames = {
  ImportedLabel: '原始标签',
  Manual: '手动标记',
  Algorithm: '算法结果',
};
const sourceColors = {
  ImportedLabel: '#65d2bd',
  Manual: '#edc56d',
  Algorithm: '#51c4e9',
};
const qualityNames = {
  Pending: '待判断',
  Accepted: '已采纳',
  Rejected: '已忽略',
};
const relationNames = {
  Current: '范围一致',
  Changed: '来源范围有差异',
  Missing: '来源已删除',
  Unavailable: '来源不可读',
};
const storageKey = 'signalstudio-annotation-u0-demo-v1';
const margins = { left: 72, right: 22, top: 16, bottom: 38 };
let state = Model.createState();
let draft = null;
let gesture = null;
let modal = null;
let manualTool = false;
let auxMode = 'wave';
let windowFocused = true;
let toastTimer = 0;
let publishedCount = 0;
let treeContentKey = '';
let tableContentKey = '';
let deferredRender = false;
let logItems = ['U0 交互演示已载入；不读取 IQ，不运行模型或 DDC。'];

function sourceBadge(annotation) {
  return /* HTML */ `<span class="badge ${annotation.sourceKind}"
    >● ${sourceNames[annotation.sourceKind]}</span
  >`;
}

function currentRange(annotation) {
  return (
    Model.segment(state, annotation)?.range ?? annotation?.observation ?? null
  );
}

function tell(message, record = false) {
  $('#statusText').textContent = message;
  $('#toast').textContent = message;
  $('#toast').classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => $('#toast').classList.remove('show'), 2600);
  if (record) {
    logItems.unshift(
      `${new Date().toLocaleTimeString('zh-CN', { hour12: false })} ${message}`
    );
    logItems = logItems.slice(0, 18);
    renderLog();
  }
}

function run(action, message, { locate = false } = {}) {
  try {
    action();
    render({ reveal: locate });
    if (message) tell(message, true);
    return true;
  } catch (error) {
    tell(error.message);
    return false;
  }
}

function renderLog() {
  $('#operationLog').innerHTML = logItems
    .map(item => /* HTML */ `<div>${escapeHtml(item)}</div>`)
    .join('');
}

function render({ reveal = false } = {}) {
  deferredRender = false;
  const sourceFile = Model.file(state);
  const matches = Model.visibleAnnotations(state);
  const count = sourceFile.annotations.filter(
    annotation => !annotation.deleted
  ).length;
  $('#contextSummary').textContent =
    `${sourceFile.name} · Fs ${number(sourceFile.sampleRateHz / 1e6, 0)} MS/s`;
  $('#scopeSummary').textContent = `${state.scenario} · file ${sourceFile.id}`;
  $('#prefixSummary').textContent =
    `可用 ${number((sourceFile.availableSamples / sourceFile.sampleRateHz) * 1000)} / ${number((sourceFile.totalSamples / sourceFile.sampleRateHz) * 1000)} ms`;
  const counts = `匹配 ${matches.length} / 总 ${count} / 已选 ${sourceFile.selection.ids.length}`;
  $('#treeCounts').textContent = counts;
  $('#recordCounts').textContent = counts;
  $('#sourceFilter').value = state.filters.source;
  $('#qualityFilter').value = state.filters.quality;
  if (document.activeElement !== $('#scoreFilter'))
    $('#scoreFilter').value = state.filters.score;
  $$('[data-layer]').forEach(input => {
    input.checked = state.filters.layers[input.dataset.layer];
  });
  $('#fileList').innerHTML = state.files
    .map(
      item =>
        /* HTML */ `<button
          class="file-row ${item.id === sourceFile.id ? 'active' : ''}"
          data-file="${item.id}"
        >
          <span>▾ ${escapeHtml(item.name)}</span
          ><small
            >${number(item.sampleRateHz / 1e6, 0)} MS/s ·
            ${number(item.centerFrequencyHz / 1e6, 0)} MHz ·
            ${number((item.totalSamples / item.sampleRateHz) * 1000, 0)}
            ms</small
          >
        </button>`
    )
    .join('');
  renderTree(matches, reveal);
  if (!draft) renderTable(matches, reveal);
  renderInspector();
  renderChannels();
  renderLog();
  $('#undoButton').disabled = state.undo.length === 0;
  $('#redoButton').disabled = state.redo.length === 0;
  $('#treeDelete').disabled = $('#tableDelete').disabled =
    sourceFile.selection.ids.length === 0;
  $('#manualTool').classList.toggle('active', manualTool);
  $('#spectrogramMode').classList.toggle(
    'active',
    state.mode === 'spectrogram'
  );
  $('#waterfallMode').classList.toggle('active', state.mode === 'waterfall');
  $('#auxWave').classList.toggle('active', auxMode === 'wave');
  $('#auxPsd').classList.toggle('active', auxMode === 'psd');
  const view = sourceFile.view;
  $('#viewSummary').textContent =
    `${number(view.startMs)}–${number(view.endMs)} ms · RF ${number(view.lowerMHz)}–${number(view.upperMHz)} MHz · ${manualTool ? '拖动空白区域持续创建手动标记' : '单击选择 · 拖动当前段修改 · 空白拖框缩放 · 坐标轴拖动平移'}`;
  drawPlots();
}

function renderTree(matches, reveal) {
  const openGroups = new Map(
    $$('#treePanel details').map(item => [item.dataset.group, item.open])
  );
  const selection = Model.file(state).selection;
  const active = Model.active(state);
  const content = Object.keys(sourceNames)
    .map(kind => {
      const rows = matches.filter(annotation => annotation.sourceKind === kind);
      const selectedIndex = rows.findIndex(
        annotation => annotation.id === selection.activeId
      );
      if (selectedIndex >= state.treeLimit[kind])
        state.treeLimit[kind] = selectedIndex + 1;
      const shown = rows.slice(0, state.treeLimit[kind]);
      const expanded =
        (reveal && active?.sourceKind === kind) ||
        openGroups.get(kind) !== false;
      return /* HTML */ `<details
        class="tree-group"
        data-group="${kind}"
        ${expanded ? 'open' : ''}
      >
        <summary>${sourceNames[kind]} (${rows.length})</summary>
        ${shown
          .map(
            annotation =>
              /* HTML */ `<button
                class="tree-item ${selection.ids.includes(annotation.id)
                  ? 'selected'
                  : ''} ${selection.activeId === annotation.id
                  ? 'current'
                  : ''} ${selection.hoverId === annotation.id ? 'hovered' : ''}"
                data-annotation="${annotation.id}"
                data-surface="tree"
                role="treeitem"
                aria-selected="${selection.ids.includes(annotation.id)}"
                title="${escapeHtml(annotation.name)}"
              >
                <span
                  class="source-dot"
                  style="background:${sourceColors[kind]}"
                ></span
                ><span class="tree-label">${escapeHtml(annotation.name)}</span
                ><span class="segment-hint"
                  >${annotation.segments.length
                    ? `${annotation.segments.length}段`
                    : '未知时间'}</span
                >
              </button>`
          )
          .join('')}${rows.length > shown.length
          ? /* HTML */ `<button class="tree-more" data-more="${kind}">
              加载后 40 条 · 已显示 ${shown.length}/${rows.length}
            </button>`
          : ''}
      </details>`;
    })
    .join('');
  const key = JSON.stringify([
    state.activeFileId,
    matches.map(item => [item.id, item.name, item.segments.length]),
    state.treeLimit,
  ]);
  if (key !== treeContentKey) {
    $('#treePanel').innerHTML = content;
    treeContentKey = key;
  }
  $$('#treePanel [data-annotation]').forEach(element => {
    const id = element.dataset.annotation;
    element.classList.toggle('selected', selection.ids.includes(id));
    element.classList.toggle('current', selection.activeId === id);
    element.classList.toggle('hovered', selection.hoverId === id);
    element.setAttribute('aria-selected', selection.ids.includes(id));
  });
  if (reveal && active)
    $(`#treePanel [data-group="${active.sourceKind}"]`).open = true;
  if (reveal) $('#treePanel .current')?.scrollIntoView({ block: 'nearest' });
}

function renderTable(matches, reveal) {
  const sourceFile = Model.file(state);
  const selection = sourceFile.selection;
  const maxPage = Math.max(0, Math.ceil(matches.length / state.pageSize) - 1);
  state.page = Math.min(state.page, maxPage);
  const rows = matches.slice(
    state.page * state.pageSize,
    (state.page + 1) * state.pageSize
  );
  const content =
    rows
      .map(annotation => {
        const part = Model.segment(state, annotation);
        const display = Model.toDisplayRange(
          sourceFile,
          currentRange(annotation)
        );
        const segmentIndex = annotation.segments.findIndex(
          item => item.id === part?.id
        );
        const channels = sourceFile.channels.filter(
          channel => channel.annotationId === annotation.id
        );
        const cell = (field, value, editable = true) =>
          /* HTML */ `<td
            class="numeric"
            ${editable ? `data-field="${field}" tabindex="0"` : ''}
          >
            ${escapeHtml(value)}
          </td>`;
        return /* HTML */ `<tr
          data-annotation="${annotation.id}"
          data-surface="table"
          class="${selection.ids.includes(annotation.id)
            ? 'selected'
            : ''} ${selection.activeId === annotation.id
            ? 'current'
            : ''} ${selection.hoverId === annotation.id ? 'hovered' : ''}"
          aria-selected="${selection.ids.includes(annotation.id)}"
        >
          <td>
            <input
              type="checkbox"
              data-row-check="${annotation.id}"
              ${selection.ids.includes(annotation.id) ? 'checked' : ''}
              aria-label="选择 ${escapeHtml(annotation.name)}"
            />
          </td>
          <td data-field="name" tabindex="0">
            ${escapeHtml(annotation.name)}<span class="cell-id"
              >${annotation.id} · r${annotation.revisionNumber}</span
            >
          </td>
          <td>${sourceBadge(annotation)}</td>
          <td>
            ${part
              ? `第 ${segmentIndex + 1}/${annotation.segments.length} 段`
              : '时间未定位'}
          </td>
          ${cell(
            'startMs',
            part ? number(display.startMs, 4) : '—',
            Boolean(part)
          )}${cell(
            'endMs',
            part ? number(display.endMs, 4) : '—',
            Boolean(part)
          )}${cell('centerMHz', number(display.centerMHz, 6))}${cell(
            'bandwidthKHz',
            number(display.bandwidthKHz, 3)
          )}
          <td class="numeric">
            ${annotation.score === null ? '—' : number(annotation.score, 2)}
          </td>
          <td>
            <span class="quality ${annotation.reviewStatus}"
              >${qualityNames[annotation.reviewStatus]}</span
            >
          </td>
          <td>${channels.length ? `${channels.length} 个快照` : '—'}</td>
        </tr>`;
      })
      .join('') ||
    '<tr><td colspan="11" class="empty">当前筛选没有标记</td></tr>';
  const key = JSON.stringify([
    state.activeFileId,
    rows.map(item => [
      item.id,
      item.revisionId,
      item.reviewStatus,
      item.activeSegmentId,
    ]),
    sourceFile.channels.length,
  ]);
  if (key !== tableContentKey || $('[data-cell-editor]')) {
    $('#annotationRows').innerHTML = content;
    tableContentKey = key;
  }
  $$('#annotationRows [data-annotation]').forEach(element => {
    const id = element.dataset.annotation;
    element.classList.toggle('selected', selection.ids.includes(id));
    element.classList.toggle('current', selection.activeId === id);
    element.classList.toggle('hovered', selection.hoverId === id);
    element.setAttribute('aria-selected', selection.ids.includes(id));
    element.querySelector('[data-row-check]').checked =
      selection.ids.includes(id);
  });
  $('#pageSummary').textContent =
    `${state.page + 1}/${maxPage + 1} 页 · 每页 ${state.pageSize}`;
  $('#previousPage').disabled = state.page === 0;
  $('#nextPage').disabled = state.page === maxPage;
  $('#selectAllCheckbox').checked =
    matches.length > 0 &&
    matches.every(annotation => selection.ids.includes(annotation.id));
  $('#selectAllCheckbox').indeterminate =
    selection.ids.length > 0 && !$('#selectAllCheckbox').checked;
  if (reveal) revealTableRow();
  $('#draftNotice').textContent =
    '双击 / F2 编辑 · Enter / Tab 提交 · Esc 取消 · 来源原文始终保留';
}

function revealTableRow() {
  const row = $('#annotationRows .current');
  if (!row) return;
  const scroller = $('#tableScroll');
  const viewport = scroller.getBoundingClientRect();
  const bounds = row.getBoundingClientRect();
  const headerHeight = $('#annotationTable thead').getBoundingClientRect()
    .height;
  if (bounds.top < viewport.top + headerHeight) {
    scroller.scrollTop += bounds.top - viewport.top - headerHeight;
  } else if (bounds.bottom > viewport.bottom) {
    scroller.scrollTop += bounds.bottom - viewport.bottom;
  }
}

function renderInspector() {
  const annotation = Model.active(state);
  if (!annotation) {
    $('#annotationInspector').innerHTML =
      '<h3>未选中对象</h3><p class="help">在时频图、表格或左树单击标记，三处显示同一选择。Ctrl 增减，Shift 连续选择。</p>';
    return;
  }
  const sourceFile = Model.file(state);
  const part = Model.segment(state);
  const range = Model.toDisplayRange(sourceFile, currentRange(annotation));
  $('#annotationInspector').innerHTML = /* HTML */ `<div
      class="inspector-title"
    >
      ${sourceBadge(annotation)}<span class="spacer"></span
      ><span class="quality ${annotation.reviewStatus}"
        >${qualityNames[annotation.reviewStatus]}</span
      >
    </div>
    <strong>${escapeHtml(annotation.name)}</strong>
    <div class="identity">
      ${annotation.id} · 当前修订 r${annotation.revisionNumber}<br />原始来源
      ${annotation.sourceKind} · ${annotation.sourceIdentity}
    </div>
    ${!part
      ? '<div class="notice">时间未定位：斜线区域仅表示观察支持。可直接编辑频率；创建通道时另选人工提取时段，不写入算法活动段。</div>'
      : '<div class="notice">仅当前活动段显示控制点；拖动和数值编辑自动修订，保留原始证据。</div>'}
    <dl class="range-info">
      <div>
        <dt>${part ? '当前段起止 / ms' : '观察支持 / ms（非活动段）'}</dt>
        <dd>${number(range.startMs, 4)}–${number(range.endMs, 4)}</dd>
      </div>
      <div>
        <dt>中心频率 / MHz</dt>
        <dd>${number(range.centerMHz, 6)}</dd>
      </div>
      <div>
        <dt>带宽 / kHz</dt>
        <dd>${number(range.bandwidthKHz)}</dd>
      </div>
      <div>
        <dt>选择</dt>
        <dd>
          ${sourceFile.selection.ids.length} 对象 ·
          ${annotation.segments.length} 活动段
        </dd>
      </div>
    </dl>
    <div class="inspector-actions">
      <button data-action="rename">改名</button
      ><button data-action="note">备注</button
      ><button data-action="range">
        ${part ? '完整范围编辑…' : '频率范围编辑…'}</button
      ><button data-action="locate">定位</button
      ><button data-action="channel" class="primary">创建窄带通道…</button>
    </div>
    <div class="inspector-actions">
      <details class="menu quality-menu">
        <summary>质量标记 ▾（可选）</summary>
        <div>
          <button data-quality="Accepted">已采纳</button
          ><button data-quality="Rejected">已忽略</button
          ><button data-quality="Pending">待判断</button>
        </div>
      </details>
      <button data-action="restore">恢复原始范围</button>
    </div>
    <p class="help" style="margin-top:9px">
      质量仅用于筛选、导出和复查，不限制编辑或通道创建。改名和备注保留质量；几何变化后恢复待判断。
    </p>
    <p class="detail-note">${escapeHtml(annotation.note || '暂无备注')}</p>
    <h3 style="margin-top:15px">活动段 ${annotation.segments.length}</h3>
    <div class="segment-list">
      ${annotation.segments
        .map((item, index) => {
          const display = Model.toDisplayRange(sourceFile, item.range);
          return /* HTML */ `<button
            class="segment-choice ${item.id === part?.id ? 'active' : ''}"
            data-segment="${item.id}"
          >
            第 ${index + 1}/${annotation.segments.length} 段 ·
            ${number(display.startMs)}–${number(display.endMs)} ms<br />${number(
              display.lowerMHz,
              5
            )}–${number(display.upperMHz, 5)}
            MHz
          </button>`;
        })
        .join('') ||
      '<p class="help">没有定位活动段；人工提取时段不会出现在这里。</p>'}
    </div>
    <div class="segment-actions">
      ${part
        ? '<button data-action="split-segment">拆分当前段</button><button data-action="merge-segment">合并相邻段</button><button data-action="delete-segment" class="danger-quiet">删除当前段</button>'
        : ''}<button data-action="add-segment">人工添加活动段…</button>
    </div>
    <details class="origin-details">
      <summary>查看不可变原始来源 / 证据</summary>
      <pre>${escapeHtml(JSON.stringify(annotation.origin, null, 2))}</pre>
    </details>`;
}

function renderChannels() {
  const sourceFile = Model.file(state);
  const makeCard = channel => {
    const relation = Model.channelRelation(sourceFile, channel);
    const range = Model.toDisplayRange(sourceFile, channel.range);
    return /* HTML */ `<div class="channel-card" data-channel="${channel.id}">
      <strong>${escapeHtml(channel.name)}</strong><br /><span
        class="relation ${relation}"
        >${relationNames[relation]}</span
      >
      ·
      ${channel.scopeOrigin === 'UserRange' ? '人工提取范围' : '绑定活动段'}<br />${number(
        range.startMs
      )}–${number(range.endMs)}
      ms · ${number(range.centerMHz, 5)} MHz<br />${escapeHtml(
        channel.annotationId
      )}${channel.segmentId
        ? ` / ${escapeHtml(channel.segmentId)}`
        : ' / 无 segmentId'}<br /><span class="help"
        >创建时快照保留；不重算、不连带删除</span
      >
    </div>`;
  };
  $('#channelCount').textContent = sourceFile.channels.length;
  $('#treeChannels').innerHTML =
    sourceFile.channels
      .map(
        channel =>
          /* HTML */ `<div class="tree-channel">
            ◇ ${escapeHtml(channel.name)}<br /><span
              class="relation ${Model.channelRelation(sourceFile, channel)}"
              >${relationNames[
                Model.channelRelation(sourceFile, channel)
              ]}</span
            >
          </div>`
      )
      .join('') || '<p class="help">尚无通道</p>';
  const linked = sourceFile.channels.filter(
    channel => channel.annotationId === Model.active(state)?.id
  );
  $('#channelList').innerHTML =
    linked.map(makeCard).join('') ||
    '<p class="help">当前对象尚无通道。质量状态不作为前置条件。</p>';
}

function selectAnnotation(id, event = {}, segmentId = null) {
  if (draft && !finishCell(true)) return;
  run(
    () => {
      Model.select(state, id, {
        ctrlKey: event.ctrlKey || event.metaKey,
        shiftKey: event.shiftKey,
        orderedIds: Model.visibleAnnotations(state).map(
          annotation => annotation.id
        ),
        segmentId: segmentId ?? undefined,
      });
      Model.ensurePageForActive(state);
    },
    '',
    { locate: true }
  );
}

function setHover(id = null, segmentId = null) {
  const selection = Model.file(state).selection;
  if (selection.hoverId === id && selection.hoverSegmentId === segmentId)
    return;
  selection.hoverId = id;
  selection.hoverSegmentId = segmentId;
  $$('[data-annotation][data-surface]').forEach(element =>
    element.classList.toggle('hovered', element.dataset.annotation === id)
  );
  drawMarkers();
}

function canvasContext(element) {
  const rect = element.getBoundingClientRect();
  const ratio = devicePixelRatio || 1;
  element.width = Math.max(1, Math.round(rect.width * ratio));
  element.height = Math.max(1, Math.round(rect.height * ratio));
  const context = element.getContext('2d');
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.fillStyle = '#0a1727';
  context.fillRect(0, 0, rect.width, rect.height);
  return { context, width: rect.width, height: rect.height };
}

function plotGeometry() {
  const rect = $('#plotArea').getBoundingClientRect();
  return {
    left: margins.left,
    top: margins.top,
    width: Math.max(1, rect.width - margins.left - margins.right),
    height: Math.max(1, rect.height - margins.top - margins.bottom),
    viewport: { x: rect.x, y: rect.y, width: rect.width, height: rect.height },
  };
}

function plotPoint(timeMs, frequencyMHz) {
  const area = plotGeometry();
  const view = Model.file(state).view;
  const t = (timeMs - view.startMs) / (view.endMs - view.startMs);
  const f = (frequencyMHz - view.lowerMHz) / (view.upperMHz - view.lowerMHz);
  return state.mode === 'waterfall'
    ? { x: area.left + f * area.width, y: area.top + t * area.height }
    : { x: area.left + t * area.width, y: area.top + (1 - f) * area.height };
}

function physicalPoint(x, y) {
  const area = plotGeometry();
  const view = Model.file(state).view;
  const u = (x - area.left) / area.width;
  const v = (y - area.top) / area.height;
  return {
    timeMs:
      view.startMs +
      (state.mode === 'waterfall' ? v : u) * (view.endMs - view.startMs),
    frequencyMHz:
      view.lowerMHz +
      (state.mode === 'waterfall' ? u : 1 - v) *
        (view.upperMHz - view.lowerMHz),
  };
}

function rangeRect(range) {
  const display = Model.toDisplayRange(Model.file(state), range);
  const first = plotPoint(display.startMs, display.upperMHz);
  const second = plotPoint(display.endMs, display.lowerMHz);
  return {
    x: Math.min(first.x, second.x),
    y: Math.min(first.y, second.y),
    width: Math.abs(first.x - second.x),
    height: Math.abs(first.y - second.y),
  };
}

function drawPlots() {
  drawNavigation();
  drawAuxiliary();
  drawSpectrogram();
  drawMarkers();
}

function drawNavigation() {
  const { context, width, height } = canvasContext($('#navCanvas'));
  const sourceFile = Model.file(state);
  const left = 46;
  const usable = width - 62;
  const fraction = sourceFile.availableSamples / sourceFile.totalSamples;
  context.fillStyle = '#24303d';
  context.fillRect(
    left + usable * fraction,
    0,
    usable * (1 - fraction),
    height
  );
  context.strokeStyle = '#48bfe9';
  context.lineWidth = 1;
  context.beginPath();
  for (let index = 0; index <= 750 * fraction; index += 1) {
    const x = left + (index / 750) * usable;
    const y =
      height * 0.65 -
      (Math.sin(index * 13.1) ** 12 * 9 + Math.sin(index / 17) * 3);
    if (index === 0) context.moveTo(x, y);
    else context.lineTo(x, y);
  }
  context.stroke();
  context.strokeStyle = '#4f9bbf';
  const duration = (sourceFile.totalSamples / sourceFile.sampleRateHz) * 1000;
  context.fillStyle = '#42c5ed15';
  const x = left + (sourceFile.view.startMs / duration) * usable;
  const w =
    ((sourceFile.view.endMs - sourceFile.view.startMs) / duration) * usable;
  context.fillRect(x, 2, w, height - 15);
  context.strokeRect(x, 2, w, height - 15);
  context.fillStyle = '#8cabc3';
  context.font = '10px Consolas';
  context.fillText('0 ms', 5, height - 4);
  context.fillText(`${number(duration, 1)} ms`, width - 64, height - 4);
}

function drawAuxiliary() {
  const { context, width, height } = canvasContext($('#auxCanvas'));
  const left = 55;
  const top = 10;
  context.strokeStyle = '#23374a';
  context.font = '10px Consolas';
  context.fillStyle = '#7c9db7';
  for (let index = 0; index <= 4; index += 1) {
    const y = top + (index / 4) * (height - 30);
    context.beginPath();
    context.moveTo(left, y);
    context.lineTo(width - 15, y);
    context.stroke();
    context.fillText(
      auxMode === 'psd' ? String(-80 - index * 15) : number(1 - index / 2, 1),
      12,
      y + 3
    );
  }
  context.strokeStyle = '#52bfea';
  context.beginPath();
  for (let index = 0; index < 900; index += 1) {
    const u = index / 899;
    const signal =
      auxMode === 'psd'
        ? 0.78 -
          0.42 * Math.exp(-(((u - 0.62) / 0.014) ** 2)) -
          0.25 * Math.exp(-(((u - 0.35) / 0.017) ** 2)) +
          Math.sin(index * 18) * 0.018
        : 0.5 +
          Math.sin(index * 0.47) * (0.08 + 0.2 * Math.sin(index * 0.013) ** 8);
    const x = left + u * (width - left - 15);
    const y = top + signal * (height - 30);
    if (index === 0) context.moveTo(x, y);
    else context.lineTo(x, y);
  }
  context.stroke();
  context.fillText(
    auxMode === 'psd' ? 'RF / MHz · 合成 PSD' : '时间 / ms · 合成 IQ 波形',
    width / 2 - 65,
    height - 3
  );
}

function drawSpectrogram() {
  const { context, width, height } = canvasContext($('#specCanvas'));
  const area = plotGeometry();
  const sourceFile = Model.file(state);
  const view = sourceFile.view;
  // A small deterministic scalar image gives readable evidence without claiming IQ DSP.
  const image = document.createElement('canvas');
  image.width = 320;
  image.height = 180;
  const imageContext = image.getContext('2d');
  const pixels = imageContext.createImageData(image.width, image.height);
  for (let y = 0; y < image.height; y += 1) {
    for (let x = 0; x < image.width; x += 1) {
      const point = physicalPoint(
        area.left + (x / image.width) * area.width,
        area.top + (y / image.height) * area.height
      );
      let power = 0.08 + 0.04 * Math.sin(x * 17.3 + y * 11.4) ** 2;
      const f = point.frequencyMHz;
      const t = point.timeMs;
      power +=
        0.77 *
        Math.exp(-(((f - 2502.15) / 0.12) ** 6)) *
        ((t > 2.9 && t < 7.1) || (t > 9.4 && t < 16.6) ? 1 : 0.12);
      power +=
        0.49 *
        Math.exp(-(((f - 2497.18) / 0.31) ** 6)) *
        ((t > 4 && t < 6) || (t > 9 && t < 12) ? 1 : 0.06);
      power +=
        0.33 *
        Math.exp(-(((f - 2499.75) / 0.28) ** 4)) *
        (0.6 + 0.3 * Math.sin(t * 2) ** 2);
      power +=
        0.45 *
        Math.exp(-(((f - 2492.38) / 0.25) ** 6)) *
        (t > 1.9 && t < 17.6 ? 1 : 0.1);
      if (t > (sourceFile.availableSamples / sourceFile.sampleRateHz) * 1000)
        power = 0.015;
      const index = (y * image.width + x) * 4;
      pixels.data[index] = Math.min(255, 12 + power ** 1.4 * 225);
      pixels.data[index + 1] = Math.max(
        0,
        power > 0.65 ? (power - 0.65) * 510 : power * 15
      );
      pixels.data[index + 2] = Math.min(
        255,
        40 + Math.sin(Math.min(power, 1) * Math.PI) * 115
      );
      pixels.data[index + 3] = 255;
    }
  }
  imageContext.putImageData(pixels, 0, 0);
  context.imageSmoothingEnabled = true;
  context.drawImage(image, area.left, area.top, area.width, area.height);
  context.font = '10px Consolas';
  for (let index = 0; index <= 5; index += 1) {
    const u = index / 5;
    const x = area.left + u * area.width;
    const y = area.top + u * area.height;
    context.strokeStyle = '#6c83952b';
    context.beginPath();
    context.moveTo(x, area.top);
    context.lineTo(x, area.top + area.height);
    context.stroke();
    context.beginPath();
    context.moveTo(area.left, y);
    context.lineTo(area.left + area.width, y);
    context.stroke();
    context.fillStyle = '#9bb7cc';
    const xValue =
      state.mode === 'waterfall'
        ? view.lowerMHz + u * (view.upperMHz - view.lowerMHz)
        : view.startMs + u * (view.endMs - view.startMs);
    const yValue =
      state.mode === 'waterfall'
        ? view.startMs + u * (view.endMs - view.startMs)
        : view.upperMHz - u * (view.upperMHz - view.lowerMHz);
    context.fillText(number(xValue, 2), x - 19, area.top + area.height + 16);
    context.fillText(number(yValue, 2), 8, y + 4);
  }
  context.fillStyle = '#93b4cb';
  context.fillText(
    state.mode === 'waterfall' ? '频率 / MHz' : '时间 / ms',
    width / 2 - 28,
    height - 5
  );
  context.save();
  context.translate(10, height / 2);
  context.rotate(-Math.PI / 2);
  context.fillText(
    state.mode === 'waterfall' ? '时间 / ms' : '频率 / MHz',
    -30,
    0
  );
  context.restore();
}

function drawMarkers() {
  const area = plotGeometry();
  const sourceFile = Model.file(state);
  const selection = sourceFile.selection;
  const svg = $('#marksSvg');
  const width = area.viewport.width;
  const height = area.viewport.height;
  svg.setAttribute('viewBox', `0 0 ${width} ${height}`);
  const markers = Model.visibleAnnotations(state).flatMap(annotation => {
    const segments = annotation.segments.length
      ? annotation.segments
      : [{ id: null, range: annotation.observation }];
    return segments.map(part => {
      const preview =
        gesture?.id === annotation.id &&
        gesture?.segmentId === part.id &&
        gesture.preview;
      const rect = rangeRect(preview || part.range);
      const selected = selection.ids.includes(annotation.id);
      const current =
        selection.activeId === annotation.id &&
        (selection.segmentId === part.id || !part.id);
      const hovered =
        selection.hoverId === annotation.id &&
        (!selection.hoverSegmentId || selection.hoverSegmentId === part.id);
      const color = sourceColors[annotation.sourceKind];
      const attrs = `x="${rect.x}" y="${rect.y}" width="${Math.max(2, rect.width)}" height="${Math.max(2, rect.height)}"`;
      const handles = current
        ? (part.id
            ? ['nw', 'n', 'ne', 'e', 'se', 's', 'sw', 'w']
            : state.mode === 'waterfall'
              ? ['w', 'e']
              : ['n', 's']
          )
            .map(handle => {
              const x =
                rect.x +
                (handle.includes('w')
                  ? 0
                  : handle.includes('e')
                    ? rect.width
                    : rect.width / 2);
              const y =
                rect.y +
                (handle.includes('n')
                  ? 0
                  : handle.includes('s')
                    ? rect.height
                    : rect.height / 2);
              return /* HTML */ `<rect
                class="handle"
                data-handle="${handle}"
                x="${x - 4}"
                y="${y - 4}"
                width="8"
                height="8"
                style="cursor:${handle}-resize"
              />`;
            })
            .join('')
        : '';
      return /* HTML */ `<g
        class="marker ${selected ? 'selected' : ''} ${current
          ? 'current'
          : ''} ${hovered ? 'hovered' : ''}"
        data-annotation="${annotation.id}"
        data-segment-id="${part.id ?? ''}"
        data-surface="plot"
        ><rect
          class="source-rect"
          ${attrs}
          fill="${part.id ? color : 'url(#unknownHatch)'}"
          stroke="${color}"
        /><rect class="hover-outline" ${attrs} />${current
          ? /* HTML */ `<rect
              class="active-outline"
              x="${rect.x - 3}"
              y="${rect.y - 3}"
              width="${rect.width + 6}"
              height="${rect.height + 6}"
            />`
          : ''}${handles}${selected || hovered
          ? /* HTML */ `<text
              class="marker-label"
              x="${rect.x + 6}"
              y="${Math.max(area.top + 13, rect.y - 6)}"
              >${escapeHtml(annotation.name)}${part.id
                ? ` · 第${annotation.segments.findIndex(item => item.id === part.id) + 1}段`
                : ' · 时间未定位'}</text
            >`
          : ''}${!part.id
          ? /* HTML */ `<text
              class="unknown-text"
              x="${rect.x + 8}"
              y="${rect.y + 14}"
              >观察频带 · 非确定活动段</text
            >`
          : ''}</g
      >`;
    });
  });
  const rubber =
    ['manual', 'zoom-box'].includes(gesture?.type) && gesture.preview
      ? rangeRect(gesture.preview)
      : null;
  svg.innerHTML = /* HTML */ `<defs
      ><pattern
        id="unknownHatch"
        width="8"
        height="8"
        patternUnits="userSpaceOnUse"
        ><path
          d="M-2,2 L2,-2 M0,8 L8,0 M6,10 L10,6"
          stroke="#51c4e9"
          stroke-width="2" /></pattern
      ><clipPath id="plotClip"
        ><rect
          x="${area.left - 5}"
          y="${area.top - 5}"
          width="${area.width + 10}"
          height="${area.height + 10}" /></clipPath></defs
    ><g clip-path="url(#plotClip)"
      >${markers.join('')}${rubber
        ? /* HTML */ `<rect
            x="${rubber.x}"
            y="${rubber.y}"
            width="${rubber.width}"
            height="${rubber.height}"
            fill="#edc56d30"
            stroke="${gesture.type === 'zoom-box' ? '#65c7ec' : '#edc56d'}"
            stroke-width="2"
          />`
        : ''}</g
    >`;
}

function startCell(cell) {
  if (!cell) return;
  const row = cell.closest('[data-annotation]');
  const annotation = Model.file(state).annotations.find(
    item => item.id === row?.dataset.annotation
  );
  if (!annotation || !cell.dataset.field) return;
  if (draft && !finishCell(true)) return;
  const field = cell.dataset.field;
  selectAnnotation(annotation.id);
  const target = $(
    `#annotationRows [data-annotation="${annotation.id}"] [data-field="${field}"]`
  );
  const display = Model.toDisplayRange(
    Model.file(state),
    currentRange(annotation)
  );
  draft = {
    id: annotation.id,
    field,
    revisionId: annotation.revisionId,
    range: clone(currentRange(annotation)),
    segmentId: annotation.activeSegmentId,
    value: field === 'name' ? annotation.name : String(display[field]),
  };
  target.innerHTML = /* HTML */ `<input
    data-cell-editor
    aria-label="编辑 ${field}"
    value="${escapeHtml(draft.value)}"
  />`;
  const input = target.querySelector('input');
  input.focus();
  input.select();
  $('#draftNotice').textContent =
    '正在编辑草稿，后台刷新保留输入；Enter / Tab 或应用内焦点转移提交，Esc 取消。';
}

function finishCell(commit, nextDirection = 0, deferRender = false) {
  if (!draft) return true;
  const input = $('[data-cell-editor]');
  const pending = draft;
  const value = input?.value ?? pending.value;
  if (commit) {
    try {
      const sourceFile = Model.file(state);
      const patch = { segmentId: pending.segmentId };
      if (pending.field === 'name') patch.name = value;
      else {
        const display = Model.toDisplayRange(sourceFile, pending.range);
        const parsed = Number(value);
        if (!value.trim() || !Number.isFinite(parsed))
          throw new Error('请输入有限数值，草稿未提交。');
        if (parsed !== display[pending.field]) {
          const range = clone(pending.range);
          if (pending.field === 'startMs' || pending.field === 'endMs') {
            const key =
              pending.field === 'startMs' ? 'startSample' : 'endSample';
            range[key] = Math.round((parsed * sourceFile.sampleRateHz) / 1000);
          } else if (pending.field === 'centerMHz') {
            const delta =
              parsed * 1e6 -
              (range.frequencyLowerHz + range.frequencyUpperHz) / 2;
            range.frequencyLowerHz += delta;
            range.frequencyUpperHz += delta;
          } else {
            const center =
              (range.frequencyLowerHz + range.frequencyUpperHz) / 2;
            range.frequencyLowerHz = center - parsed * 500;
            range.frequencyUpperHz = center + parsed * 500;
          }
          if (pending.segmentId) patch.range = range;
          else
            patch.frequency = {
              lowerHz: range.frequencyLowerHz,
              upperHz: range.frequencyUpperHz,
            };
        }
      }
      Model.edit(state, pending.id, patch, pending.revisionId);
    } catch (error) {
      tell(error.message);
      input?.setAttribute('aria-invalid', 'true');
      return false;
    }
  }
  draft = null;
  if (deferRender) deferredRender = true;
  else render();
  if (commit) tell('已提交当前字段；原始来源和证据保留。', true);
  if (nextDirection) {
    const cells = $$('#annotationRows [data-field]');
    const index = cells.findIndex(
      cell =>
        cell.closest('[data-annotation]').dataset.annotation === pending.id &&
        cell.dataset.field === pending.field
    );
    const next = cells[index + nextDirection];
    if (next) startCell(next);
  }
  return true;
}

function showModal(title, content, actions, onConfirm = null, type = '') {
  closeContext();
  modal = { onConfirm, type, previousFocus: document.activeElement };
  $('#modalTitle').textContent = title;
  $('#modalContent').innerHTML = content;
  $('#modalActions').innerHTML = actions;
  $('#modalBackdrop').classList.remove('hidden');
  requestAnimationFrame(() =>
    $(
      '#modalContent input, #modalContent textarea, #modalActions button'
    )?.focus()
  );
}

function closeModal() {
  const focus = modal?.previousFocus;
  modal = null;
  $('#modalBackdrop').classList.add('hidden');
  if (focus?.isConnected) focus.focus();
}

function dialogActions(label = '应用修改') {
  return /* HTML */ `<button data-action="close-modal">取消</button
    ><button data-action="confirm-modal" class="primary" id="confirmModal">
      ${label}
    </button>`;
}

function openTextEditor(field) {
  const annotation = Model.active(state);
  if (!annotation) return tell('请先选择一个标记对象。');
  const base = annotation.revisionId;
  showModal(
    field === 'name' ? '修改名称' : '修改备注',
    /* HTML */ `<p class="dialog-source">
        ${sourceBadge(annotation)} · ${escapeHtml(annotation.id)} ·
        自动建立当前修订，质量和已有通道保持。
      </p>
      <div class="dialog-grid">
        <label class="full-row"
          >${field === 'name' ? '名称' : '备注'}${field === 'name'
            ? /* HTML */ `<input
                id="textEdit"
                value="${escapeHtml(annotation.name)}"
              />`
            : /* HTML */ `<textarea id="textEdit">
${escapeHtml(annotation.note)}</textarea
              >`}</label
        >
      </div>
      <div class="validation-errors" id="dialogErrors"></div>`,
    dialogActions(),
    () => {
      try {
        Model.edit(
          state,
          annotation.id,
          { [field]: $('#textEdit').value },
          base
        );
        closeModal();
        render();
        tell('文字修改已提交；质量及通道范围关系保持。', true);
      } catch (error) {
        $('#dialogErrors').textContent = error.message;
      }
    },
    'text'
  );
}

function rangeFields(range, timeEditable = true) {
  return /* HTML */ `<div class="dialog-grid">
    ${timeEditable
      ? /* HTML */ `<label
            >起始时间 / ms<input
              id="rangeStart"
              value="${range.startMs}" /></label
          ><label
            >结束时间 / ms<input id="rangeEnd" value="${range.endMs}"
          /></label>`
      : '<div class="notice full-row">时间保持未知，仅修改观察频率；不会生成算法活动段。</div>'}<label
      >频率下界 / MHz<input id="rangeLower" value="${range.lowerMHz}" /></label
    ><label
      >频率上界 / MHz<input id="rangeUpper" value="${range.upperMHz}"
    /></label>
  </div>`;
}

function readFinite(selector, label) {
  const text = $(selector).value;
  if (!text.trim() || !Number.isFinite(Number(text)))
    throw new Error(`${label}：请输入有限数值。`);
  return Number(text);
}

function openRangeEditor(add = false) {
  const annotation = Model.active(state);
  if (!annotation) return tell('请先选择一个标记对象。');
  const sourceFile = Model.file(state);
  const part = Model.segment(state);
  const range = add
    ? { ...sourceFile.view }
    : Model.toDisplayRange(sourceFile, currentRange(annotation));
  const base = annotation.revisionId;
  const timeEditable = add || Boolean(part);
  const originalRange = clone(currentRange(annotation));
  showModal(
    add ? '人工添加活动段' : '一次提交完整范围',
    /* HTML */ `<p class="dialog-source">
        ${sourceBadge(annotation)} · ${escapeHtml(annotation.name)}
      </p>
      ${add
        ? '<div class="notice warning">此操作显式修订标记的活动段；仅为提取数据时，请使用“创建窄带通道”的人工范围。</div>'
        : ''}${rangeFields(range, timeEditable)}
      <div class="validation-errors" id="dialogErrors"></div>`,
    dialogActions(),
    () => {
      try {
        const lowerMHz = readFinite('#rangeLower', '频率下界');
        const upperMHz = readFinite('#rangeUpper', '频率上界');
        const updated = clone(originalRange);
        if (lowerMHz !== range.lowerMHz)
          updated.frequencyLowerHz = lowerMHz * 1e6;
        if (upperMHz !== range.upperMHz)
          updated.frequencyUpperHz = upperMHz * 1e6;
        if (timeEditable) {
          const startMs = readFinite('#rangeStart', '起始时间');
          const endMs = readFinite('#rangeEnd', '结束时间');
          if (startMs !== range.startMs)
            updated.startSample = Math.round(
              (startMs * sourceFile.sampleRateHz) / 1000
            );
          if (endMs !== range.endMs)
            updated.endSample = Math.round(
              (endMs * sourceFile.sampleRateHz) / 1000
            );
        }
        const patch = timeEditable
          ? {
              range: add
                ? Model.fromDisplayRange(sourceFile, {
                    startMs: readFinite('#rangeStart', '起始时间'),
                    endMs: readFinite('#rangeEnd', '结束时间'),
                    lowerMHz,
                    upperMHz,
                  })
                : updated,
            }
          : {
              frequency: {
                lowerHz: updated.frequencyLowerHz,
                upperHz: updated.frequencyUpperHz,
              },
            };
        if (add) Model.addSegment(state, annotation.id, patch.range);
        else
          Model.edit(
            state,
            annotation.id,
            { ...patch, segmentId: part?.id },
            base
          );
        closeModal();
        render();
        tell('完整范围已原子提交；可一次撤销。', true);
      } catch (error) {
        $('#dialogErrors').textContent = error.message;
      }
    },
    'range'
  );
}

function openDelete(segmentOnly = false) {
  const annotation = Model.active(state);
  const part = Model.segment(state);
  if (segmentOnly && (!annotation || !part))
    return tell('请选择有活动段的当前对象。');
  const sourceFile = Model.file(state);
  const summary = Model.deleteSummary(state);
  if (!segmentOnly && !summary.objects) return tell('当前筛选内没有所选标记。');
  const last = segmentOnly && annotation.segments.length === 1;
  const channels = segmentOnly
    ? sourceFile.channels.filter(
        channel =>
          channel.annotationId === annotation.id &&
          (last || channel.segmentId === part.id)
      ).length
    : summary.channels;
  const title = segmentOnly
    ? last
      ? '删除最后一段将删除标记对象'
      : '删除当前活动段'
    : '删除所选标记';
  showModal(
    title,
    /* HTML */ `<p>
        仅处理当前筛选内选择。删除后可一次撤销；关联通道保留创建时快照。
      </p>
      <div class="delete-counts">
        <div>
          <strong id="deleteObjectCount"
            >${segmentOnly ? (last ? 1 : 0) : summary.objects}</strong
          >标记对象
        </div>
        <div>
          <strong id="deleteSegmentCount"
            >${segmentOnly ? 1 : summary.segments}</strong
          >活动段
        </div>
        <div><strong id="deleteChannelCount">${channels}</strong>关联通道</div>
      </div>
      <div class="notice warning">
        ${segmentOnly && !last
          ? '其它活动段保留；绑定被删除段的通道显示来源已删除。'
          : '关联通道不会删除或重算，显示来源已删除。'}删除不会写回原始标签文件。
      </div>`,
    dialogActions(segmentOnly && !last ? '删除当前段' : '删除标记'),
    () => {
      if (
        run(
          () =>
            segmentOnly
              ? Model.deleteSegment(state, annotation.id, part.id)
              : Model.deleteSelected(state),
          '删除已完成；一次撤销可恢复。'
        )
      )
        closeModal();
    },
    'delete'
  );
}

function channelForm(request, annotation) {
  const display = Model.toDisplayRange(Model.file(state), request.range);
  return /* HTML */ `<p class="dialog-source">
      ${sourceBadge(annotation)} · ${escapeHtml(annotation.name)}<br />标记
      ${escapeHtml(annotation.id)} · 修订
      ${escapeHtml(request.sourceRevisionId)}
    </p>
    <div
      class="notice ${request.scopeOrigin === 'UserRange' ? 'warning' : ''}"
      id="channelScope"
    >
      <strong
        >${request.scopeOrigin === 'UserRange'
          ? 'UserRange · 人工提取范围'
          : `LocatedSegment · ${escapeHtml(request.segmentId)}`}</strong
      ><br />${request.scopeOrigin === 'UserRange'
        ? '默认当前视区与可用前缀的交集；不等同算法活动段，不修改检测证据。'
        : '默认当前活动段，可裁剪提取时段；不自动合并多个段。'}质量状态：${qualityNames[
        annotation.reviewStatus
      ]}（不作为创建前置条件）
    </div>
    <div class="dialog-grid">
      <label class="full-row"
        >通道名称<input
          id="channelName"
          value="${escapeHtml(request.name)}" /></label
      ><label
        >提取起始 / ms<input
          id="channelStart"
          value="${display.startMs}" /></label
      ><label
        >提取结束 / ms<input id="channelEnd" value="${display.endMs}" /></label
      ><label
        >中心频率 / MHz<input
          id="channelCenter"
          value="${request.centerFrequencyHz / 1e6}" /></label
      ><label
        >通道带宽 / kHz<input
          id="channelBandwidth"
          value="${request.bandwidthHz / 1000}" /></label
      ><label
        >输出采样率 / MS/s<input
          id="channelRate"
          value="${request.outputSampleRateHz / 1e6}" /></label
      ><label
        >滤波器<select id="channelFilter">
          <option value="standard">标准 FIR · 保护带 15%</option>
        </select></label
      >
    </div>
    <div class="inspector-actions">
      <button data-action="use-prefix">显式使用可用前缀</button>
    </div>
    <div class="channel-illustration">来源 IQ → CPU DDC / FIR → 重采样计划</div>
    <p class="help">
      本原型仅检查配置并保存演示快照，不执行真实 DDC；真实输出属于后续 U3 验收。
    </p>
    <div class="validation-errors" id="channelErrors"></div>
    <div class="help" id="dspSummary"></div>`;
}

function readChannelRequest() {
  const request = clone(modal.request);
  const sourceFile = Model.file(state);
  const center = readFinite('#channelCenter', '中心频率') * 1e6;
  const bandwidth = readFinite('#channelBandwidth', '通道带宽') * 1000;
  request.name = $('#channelName').value;
  request.centerFrequencyHz = center;
  request.bandwidthHz = bandwidth;
  request.outputSampleRateHz = readFinite('#channelRate', '输出采样率') * 1e6;
  request.filter = $('#channelFilter').value;
  // Do not normalize, clip, or silently replace invalid user input.
  request.range = {
    startSample: Math.round(
      (readFinite('#channelStart', '提取起始') * sourceFile.sampleRateHz) / 1000
    ),
    endSample: Math.round(
      (readFinite('#channelEnd', '提取结束') * sourceFile.sampleRateHz) / 1000
    ),
    frequencyLowerHz: center - bandwidth / 2,
    frequencyUpperHz: center + bandwidth / 2,
  };
  return request;
}

function validateChannelForm() {
  try {
    const request = readChannelRequest();
    const errors = Model.validateChannelRequest(state, request);
    $('#channelErrors').innerHTML = errors
      .map(
        error =>
          /* HTML */ `<div data-error="${escapeHtml(error.code)}">
            ${escapeHtml(error.field)}：${escapeHtml(error.message)}
          </div>`
      )
      .join('');
    $('#confirmModal').disabled = errors.length > 0;
    $('#dspSummary').textContent =
      `DSP 计划：${number(Model.file(state).sampleRateHz / 1e6, 2)} → ${number(request.outputSampleRateHz / 1e6, 3)} MS/s · 频移 ${number((request.centerFrequencyHz - Model.file(state).centerFrequencyHz) / 1e6, 5)} MHz · ${Math.max(0, request.range.endSample - request.range.startSample)} 输入样本`;
  } catch (error) {
    $('#channelErrors').textContent = error.message;
    $('#confirmModal').disabled = true;
  }
}

function openChannel() {
  const sourceFile = Model.file(state);
  if (sourceFile.selection.ids.length !== 1) {
    if (sourceFile.selection.ids.length === 0)
      return tell('请选择一个来源对象，再创建窄带通道。');
    const selected = sourceFile.annotations.filter(annotation =>
      sourceFile.selection.ids.includes(annotation.id)
    );
    showModal(
      '选择一个通道来源对象',
      '<p>一次创建一个通道；不会合并多个对象或多段包围盒。</p><div class="segment-list">' +
        selected
          .map(
            annotation =>
              /* HTML */ `<button data-channel-source="${annotation.id}">
                ${sourceNames[annotation.sourceKind]} ·
                ${escapeHtml(annotation.name)}
              </button>`
          )
          .join('') +
        '</div>',
      '<button data-action="close-modal">取消</button>',
      null,
      'source-picker'
    );
    return;
  }
  try {
    const request = Model.defaultChannelRequest(state);
    showModal(
      '创建窄带通道 · 演示配置',
      channelForm(request, Model.active(state)),
      dialogActions('创建演示通道快照'),
      () => {
        try {
          const channel = Model.createChannel(state, readChannelRequest());
          closeModal();
          render();
          tell(
            `演示通道 ${channel.id} 已创建；质量状态保持，未执行真实 DDC。`,
            true
          );
        } catch (error) {
          $('#channelErrors').textContent = error.message;
        }
      },
      'channel'
    );
    modal.request = request;
    validateChannelForm();
  } catch (error) {
    tell(error.message);
  }
}

function closeContext() {
  $('#contextMenu').classList.add('hidden');
}

function openContext(event, id = null, segmentId = null) {
  event.preventDefault();
  if (id && !Model.file(state).selection.ids.includes(id))
    selectAnnotation(id, {}, segmentId);
  else if (id) {
    const sourceFile = Model.file(state);
    sourceFile.selection.activeId = id;
    const annotation = sourceFile.annotations.find(item => item.id === id);
    annotation.activeSegmentId = segmentId || annotation.activeSegmentId;
    sourceFile.selection.segmentId = annotation.activeSegmentId;
    Model.ensurePageForActive(state);
    render({ reveal: true });
  }
  $('#contextMenu').innerHTML = /* HTML */ `<div class="menu-caption">
      ${Model.file(state).selection.ids.length} 个对象已选
    </div>
    <button data-action="locate">定位当前活动段</button
    ><button data-action="range">编辑当前范围…</button
    ><button data-action="channel">创建窄带通道…</button>
    <hr />
    <button data-quality="Accepted">质量 · 已采纳</button
    ><button data-quality="Rejected">质量 · 已忽略</button
    ><button data-quality="Pending">质量 · 待判断</button>
    <hr />
    <button data-action="delete" class="danger-quiet">删除所选标记…</button
    >${Model.segment(state)
      ? '<button data-action="delete-segment">删除当前活动段…</button>'
      : ''}`;
  $('#contextMenu').classList.remove('hidden');
  const rect = $('#contextMenu').getBoundingClientRect();
  $('#contextMenu').style.left =
    `${Math.min(event.clientX, innerWidth - rect.width - 8)}px`;
  $('#contextMenu').style.top =
    `${Math.min(event.clientY, innerHeight - rect.height - 8)}px`;
}

function locateCurrent() {
  const annotation = Model.active(state);
  if (!annotation) return tell('请先选择一个标记对象。');
  const sourceFile = Model.file(state);
  const display = Model.toDisplayRange(sourceFile, currentRange(annotation));
  const duration = (sourceFile.totalSamples / sourceFile.sampleRateHz) * 1000;
  const padTime = Math.max(0.15, (display.endMs - display.startMs) * 0.18);
  const padFrequency = Math.max(
    0.15,
    (display.upperMHz - display.lowerMHz) * 0.8
  );
  sourceFile.view = {
    startMs: Math.max(0, display.startMs - padTime),
    endMs: Math.min(duration, display.endMs + padTime),
    lowerMHz: Math.max(
      (sourceFile.centerFrequencyHz - sourceFile.sampleRateHz / 2) / 1e6,
      display.lowerMHz - padFrequency
    ),
    upperMHz: Math.min(
      (sourceFile.centerFrequencyHz + sourceFile.sampleRateHz / 2) / 1e6,
      display.upperMHz + padFrequency
    ),
  };
  render({ reveal: true });
}

function zoomView(factor, axis = null) {
  const sourceFile = Model.file(state);
  const view = sourceFile.view;
  const duration = (sourceFile.totalSamples / sourceFile.sampleRateHz) * 1000;
  const timeFactor = axis === 'frequency' ? 1 : factor;
  const frequencyFactor = axis === 'time' ? 1 : factor;
  const timeWidth = Math.min(
    duration,
    Math.max(0.05, (view.endMs - view.startMs) * timeFactor)
  );
  const frequencyWidth = Math.min(
    sourceFile.sampleRateHz / 1e6,
    Math.max(0.02, (view.upperMHz - view.lowerMHz) * frequencyFactor)
  );
  const timeStart = Math.min(
    duration - timeWidth,
    Math.max(0, (view.startMs + view.endMs - timeWidth) / 2)
  );
  const frequencyMin =
    (sourceFile.centerFrequencyHz - sourceFile.sampleRateHz / 2) / 1e6;
  const frequencyStart = Math.min(
    frequencyMin + sourceFile.sampleRateHz / 1e6 - frequencyWidth,
    Math.max(frequencyMin, (view.lowerMHz + view.upperMHz - frequencyWidth) / 2)
  );
  sourceFile.view = {
    startMs: timeStart,
    endMs: timeStart + timeWidth,
    lowerMHz: frequencyStart,
    upperMHz: frequencyStart + frequencyWidth,
  };
  render();
}

function saveDemo() {
  if (draft && !finishCell(true)) return;
  localStorage.setItem(storageKey, JSON.stringify(Model.save(state)));
  tell('演示状态已保存在本机浏览器；不是原生工程文件。', true);
}

function resetScenario() {
  draft = null;
  gesture = null;
  manualTool = false;
  closeModal();
  closeContext();
  state = Model.createState($('#scenario').value);
  logItems = [`已载入 ${state.scenario} 合成评审场景。`];
  render({ reveal: true });
}

function performAction(action) {
  closeContext();
  $$('.menu[open]').forEach(menu => {
    menu.open = false;
  });
  if (
    !['close-modal', 'confirm-modal', 'publish'].includes(action) &&
    draft &&
    !finishCell(true)
  )
    return;
  const annotation = Model.active(state);
  switch (action) {
    case 'reset':
      resetScenario();
      break;
    case 'select-all':
      run(
        () => Model.selectAll(state),
        '已选择当前筛选的全部匹配结果，包含其它页。'
      );
      break;
    case 'clear-selection':
      run(() => Model.clearSelection(state));
      break;
    case 'delete':
      openDelete();
      break;
    case 'delete-segment':
      openDelete(true);
      break;
    case 'rename':
      openTextEditor('name');
      break;
    case 'note':
      openTextEditor('note');
      break;
    case 'range':
      openRangeEditor();
      break;
    case 'add-segment':
      openRangeEditor(true);
      break;
    case 'restore':
      if (annotation)
        run(
          () => Model.restoreOriginRange(state, annotation.id),
          '原始范围已恢复；修订历史保留。'
        );
      break;
    case 'split-segment':
      if (annotation)
        run(
          () =>
            Model.splitSegment(
              state,
              annotation.id,
              annotation.activeSegmentId
            ),
          '当前段已在中点拆分；可一次撤销。'
        );
      break;
    case 'merge-segment':
      if (annotation)
        run(
          () =>
            Model.mergeAdjacent(
              state,
              annotation.id,
              annotation.activeSegmentId
            ),
          '当前段与相邻段合并；绑定被移除段的通道保留快照。'
        );
      break;
    case 'undo':
      run(() => Model.undo(state), '已撤销上一标记命令。');
      break;
    case 'redo':
      run(() => Model.redo(state), '已重做上一标记命令。');
      break;
    case 'previous-page':
      state.page = Math.max(0, state.page - 1);
      render();
      break;
    case 'next-page':
      state.page += 1;
      render();
      break;
    case 'channel':
      openChannel();
      break;
    case 'locate':
      locateCurrent();
      break;
    case 'zoom':
      zoomView(0.65);
      break;
    case 'fit': {
      const sourceFile = Model.file(state);
      sourceFile.view = {
        startMs: 0,
        endMs: (sourceFile.availableSamples / sourceFile.sampleRateHz) * 1000,
        lowerMHz:
          (sourceFile.centerFrequencyHz - sourceFile.sampleRateHz / 2) / 1e6,
        upperMHz:
          (sourceFile.centerFrequencyHz + sourceFile.sampleRateHz / 2) / 1e6,
      };
      render();
      break;
    }
    case 'spectrogram':
      state.mode = 'spectrogram';
      render();
      break;
    case 'waterfall':
      state.mode = 'waterfall';
      render();
      break;
    case 'aux-wave':
      auxMode = 'wave';
      render();
      break;
    case 'aux-psd':
      auxMode = 'psd';
      render();
      break;
    case 'manual':
      manualTool = !manualTool;
      render();
      tell(
        manualTool
          ? '在空白图区拖动创建标记，可连续创建；Esc 退出。'
          : '已退出手动标记工具。'
      );
      break;
    case 'close-modal':
      closeModal();
      break;
    case 'confirm-modal':
      modal?.onConfirm?.();
      break;
    case 'use-prefix': {
      const sourceFile = Model.file(state);
      $('#channelStart').value = 0;
      $('#channelEnd').value =
        (sourceFile.availableSamples / sourceFile.sampleRateHz) * 1000;
      validateChannelForm();
      break;
    }
    case 'save':
      saveDemo();
      break;
    case 'reopen':
      run(
        () => {
          const text = localStorage.getItem(storageKey);
          if (!text) throw new Error('尚未保存演示状态。');
          state = Model.load(text);
          draft = null;
          $('#scenario').value = state.scenario;
        },
        '已重开浏览器演示；未读取原生工程。',
        { locate: true }
      );
      break;
    case 'export': {
      const blob = new Blob([JSON.stringify(Model.save(state), null, 2)], {
        type: 'application/json',
      });
      const url = URL.createObjectURL(blob);
      const link = document.createElement('a');
      link.href = url;
      link.download = 'signalstudio-annotation-u0-demo.json';
      link.click();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
      break;
    }
    case 'publish': {
      const sourceFile = Model.file(state);
      const template = sourceFile.annotations.find(
        item => item.id === 'algo-ignored'
      );
      if (!template) break;
      const result = clone(template);
      do {
        publishedCount += 1;
      } while (
        state.files.some(item =>
          item.annotations.some(
            existing => existing.id === `published-${publishedCount}`
          )
        )
      );
      result.id = `published-${publishedCount}`;
      result.name = `后台合成结果 ${publishedCount}`;
      result.deleted = false;
      result.reviewStatus = 'Pending';
      result.segments = result.segments.map((part, index) => ({
        ...part,
        id: `${result.id}-s${index + 1}`,
      }));
      result.activeSegmentId = result.segments[0].id;
      result.revisionNumber = 0;
      result.revisionId = `${result.id}-r0`;
      result.revisionHistory = [];
      result.qualityHistory = [];
      result.origin = Object.freeze({
        name: result.name,
        segments: clone(result.segments),
        observation: null,
        evidence: {
          demoOnly: true,
          description: '显式追加的合成后台发布结果。',
        },
      });
      sourceFile.annotations.push(result);
      render();
      tell('已追加一条合成结果；当前选择和未提交草稿保持。', true);
      break;
    }
    case 'help':
      showModal(
        'U0 审核说明',
        '<p>单击选择，Ctrl 增减，Shift 连选；图、表、左树共用选择。全选覆盖全部匹配页，改变筛选后剔除不匹配选择。</p><div class="notice">三来源均可拖动当前段或双击表格编辑；修改自动修订。时间未定位对象仅可编辑频率，不伪造活动段。</div><p>质量标记可选。创建通道选择明确活动段或人工提取范围，必要的源身份、可读范围及 DSP 条件仍检查。</p><div class="notice warning">所有图谱、标记和通道均为交互演示；浏览器测试不证明原生 Qt、模型推理、真实 IQ 或 CPU DDC。</div>',
        '<button data-action="close-modal" class="primary">知道了</button>'
      );
      break;
    default:
      break;
  }
}

document.addEventListener('click', event => {
  const action = event.target.closest('[data-action]');
  if (action) {
    performAction(action.dataset.action);
    return;
  }
  const quality = event.target.closest('[data-quality]');
  if (quality) {
    run(
      () => Model.setQuality(state, quality.dataset.quality),
      '质量标记已更新；不影响通道创建或已有快照。'
    );
    closeContext();
    $$('.quality-menu').forEach(menu => {
      menu.open = false;
    });
    return;
  }
  const source = event.target.closest('[data-channel-source]');
  if (source) {
    closeModal();
    selectAnnotation(source.dataset.channelSource);
    openChannel();
    return;
  }
  const file = event.target.closest('[data-file]');
  if (file) {
    draft = null;
    gesture = null;
    run(() => Model.switchFile(state, file.dataset.file));
    return;
  }
  const more = event.target.closest('[data-more]');
  if (more) {
    state.treeLimit[more.dataset.more] += 40;
    render();
    return;
  }
  const part = event.target.closest('[data-segment]');
  if (part) {
    selectAnnotation(Model.active(state).id, {}, part.dataset.segment);
    return;
  }
  const sort = event.target.closest('[data-sort]');
  if (sort) {
    run(
      () => {
        Model.sortBy(
          state,
          { start: 'startMs', center: 'centerMHz' }[sort.dataset.sort] ||
            sort.dataset.sort
        );
        Model.ensurePageForActive(state);
      },
      '',
      { locate: true }
    );
    return;
  }
  const row = event.target.closest('[data-annotation][data-surface]');
  if (
    row &&
    row.dataset.surface !== 'plot' &&
    !event.target.closest('[data-cell-editor]')
  ) {
    const checkbox = event.target.closest('[data-row-check]');
    selectAnnotation(row.dataset.annotation, {
      ctrlKey: checkbox ? true : event.ctrlKey,
      metaKey: event.metaKey,
      shiftKey: event.shiftKey,
    });
  }
  if (!event.target.closest('#contextMenu')) closeContext();
});

document.addEventListener('dblclick', event => {
  const cell = event.target.closest('td[data-field]');
  if (cell) {
    startCell(cell);
    return;
  }
  const row = event.target.closest('[data-annotation]');
  if (row && row.dataset.surface !== 'plot') {
    selectAnnotation(row.dataset.annotation);
    locateCurrent();
  }
});

document.addEventListener('contextmenu', event => {
  const row = event.target.closest('[data-annotation][data-surface]');
  if (row)
    openContext(event, row.dataset.annotation, row.dataset.segmentId || null);
  else if (event.target.closest('#plotArea')) openContext(event);
});

document.addEventListener('mouseover', event => {
  if (gesture) return;
  const row = event.target.closest('[data-annotation][data-surface]');
  if (row && row.dataset.surface !== 'plot') setHover(row.dataset.annotation);
});
$('#treePanel').addEventListener('mouseleave', () => setHover());
$('#tableScroll').addEventListener('mouseleave', () => setHover());

$('#sourceFilter').addEventListener('change', () => {
  draft = null;
  run(() => Model.applyFilter(state, { source: $('#sourceFilter').value }));
});
$('#qualityFilter').addEventListener('change', () => {
  draft = null;
  run(() => Model.applyFilter(state, { quality: $('#qualityFilter').value }));
});
$$('[data-layer]').forEach(input =>
  input.addEventListener('change', () => {
    draft = null;
    run(() =>
      Model.applyFilter(state, {
        layers: {
          ...state.filters.layers,
          [input.dataset.layer]: input.checked,
        },
      })
    );
  })
);
$('#selectAllCheckbox').addEventListener('change', event =>
  run(() =>
    event.target.checked ? Model.selectAll(state) : Model.clearSelection(state)
  )
);
$('#scenario').addEventListener('change', resetScenario);

function commitScore(deferRender = false) {
  const value = Number($('#scoreFilter').value);
  if (
    !$('#scoreFilter').value.trim() ||
    !Number.isFinite(value) ||
    value < 0 ||
    value > 1
  ) {
    tell('最低分数须在 0–1 之间。');
    return;
  }
  if (value === state.filters.score) return;
  if (deferRender) {
    Model.applyFilter(state, { score: value });
    deferredRender = true;
  } else run(() => Model.applyFilter(state, { score: value }));
}
$('#scoreFilter').addEventListener('blur', event => {
  if (windowFocused && event.relatedTarget && !deferredRender) commitScore();
});

document.addEventListener('input', event => {
  if (modal?.type === 'channel' && event.target.closest('#modalContent'))
    validateChannelForm();
});
document.addEventListener('focusout', event => {
  if (
    event.target.matches('[data-cell-editor]') &&
    event.relatedTarget &&
    windowFocused &&
    !event.relatedTarget.closest(
      '[data-file],#scenario,[data-action="publish"]'
    )
  )
    finishCell(true);
});
function cancelGesture() {
  if (!gesture) return;
  const pending = gesture;
  gesture = null;
  if (pending.type === 'pan') Model.file(state).view = pending.originalView;
  if (
    pending.pointerId !== undefined &&
    $('#plotArea').hasPointerCapture(pending.pointerId)
  ) {
    $('#plotArea').releasePointerCapture(pending.pointerId);
  }
  drawPlots();
}
window.addEventListener('blur', () => {
  windowFocused = false;
  cancelGesture();
});
window.addEventListener('focus', () => {
  windowFocused = true;
});
document.addEventListener(
  'pointerdown',
  event => {
    if (
      document.activeElement === $('#scoreFilter') &&
      event.target !== $('#scoreFilter') &&
      !event.target.closest('[data-file],#scenario')
    )
      commitScore(true);
    if (draft && event.target.closest('[data-action="publish"]')) {
      event.preventDefault();
      return;
    }
    if (
      draft &&
      !event.target.closest('[data-cell-editor]') &&
      !event.target.closest('[data-file],#scenario,[data-action="publish"]')
    )
      finishCell(true, 0, true);
  },
  true
);

// Flush after the target's click has run, so committing never removes its target.
document.addEventListener('click', () => {
  if (deferredRender) render();
});

document.addEventListener('keydown', event => {
  const input = event.target.closest('input,textarea,select');
  if (event.key === 'Escape') {
    event.preventDefault();
    if (modal) closeModal();
    else if (draft) finishCell(false);
    else if (gesture) {
      cancelGesture();
      tell('已取消拖动，原范围保持。');
    } else if (!$('#contextMenu').classList.contains('hidden')) closeContext();
    else {
      manualTool = false;
      render();
    }
    return;
  }
  if (event.target.matches('[data-cell-editor]')) {
    if (event.key === 'Enter' || event.key === 'Tab') {
      event.preventDefault();
      finishCell(true, event.key === 'Tab' ? (event.shiftKey ? -1 : 1) : 0);
    }
    return;
  }
  if (
    event.target === $('#scoreFilter') &&
    (event.key === 'Enter' || event.key === 'Tab')
  ) {
    event.preventDefault();
    commitScore();
    if (event.key === 'Tab') $('#sourceFilter').focus();
    return;
  }
  if (modal) {
    if (
      event.key === 'Enter' &&
      event.target.matches('input') &&
      !$('#confirmModal')?.disabled
    ) {
      event.preventDefault();
      modal.onConfirm?.();
    }
    if (event.key === 'Tab') {
      const items = $$(
        '#modal button:not(:disabled),#modal input,#modal textarea,#modal select'
      ).filter(element => element.offsetParent !== null);
      const index = items.indexOf(document.activeElement);
      if (event.shiftKey && index === 0) {
        event.preventDefault();
        items.at(-1)?.focus();
      } else if (!event.shiftKey && index === items.length - 1) {
        event.preventDefault();
        items[0]?.focus();
      }
    }
    return;
  }
  if (input) return;
  if (
    (event.ctrlKey || event.metaKey) &&
    ['z', 'y', 's', 'a'].includes(event.key.toLowerCase())
  ) {
    event.preventDefault();
    performAction(
      { z: 'undo', y: 'redo', s: 'save', a: 'select-all' }[
        event.key.toLowerCase()
      ]
    );
    return;
  }
  if (event.key === 'Delete') {
    event.preventDefault();
    openDelete();
  }
  if (event.key === 'F2') {
    event.preventDefault();
    startCell(
      event.target.closest('td[data-field]') ||
        $('#annotationRows .current [data-field="name"]')
    );
  }
});

// Disable wheel changes in all parameter widgets; chart zoom and panel scrolling remain.
document.addEventListener(
  'wheel',
  event => {
    if (event.target.closest('select,input,textarea')) {
      event.preventDefault();
      event.target
        .closest('.inspector-scroll,.modal-content,.table-scroll')
        ?.scrollBy(0, event.deltaY);
    }
  },
  { passive: false }
);

function pointerPosition(event) {
  const rect = $('#plotArea').getBoundingClientRect();
  return { x: event.clientX - rect.x, y: event.clientY - rect.y };
}

function pointInPlot(point) {
  const area = plotGeometry();
  return (
    point.x >= area.left &&
    point.x <= area.left + area.width &&
    point.y >= area.top &&
    point.y <= area.top + area.height
  );
}

function hitCandidates(point) {
  return Model.visibleAnnotations(state).flatMap(annotation =>
    (annotation.segments.length
      ? annotation.segments
      : [{ id: null, range: annotation.observation }]
    )
      .filter(part => {
        const rect = rangeRect(part.range);
        return (
          point.x >= rect.x - 3 &&
          point.x <= rect.x + rect.width + 3 &&
          point.y >= rect.y - 3 &&
          point.y <= rect.y + rect.height + 3
        );
      })
      .map(part => ({ annotation, part }))
  );
}

$('#plotArea').addEventListener('pointerdown', event => {
  if (event.button !== 0 || modal) return;
  if (draft && !finishCell(true)) return;
  const point = pointerPosition(event);
  const area = plotGeometry();
  if (!pointInPlot(point)) {
    const axis =
      point.x < area.left &&
      point.y >= area.top &&
      point.y <= area.top + area.height
        ? 'y'
        : point.y > area.top + area.height &&
            point.x >= area.left &&
            point.x <= area.left + area.width
          ? 'x'
          : null;
    if (!axis) return;
    event.preventDefault();
    gesture = {
      type: 'pan',
      axis,
      start: point,
      originalView: clone(Model.file(state).view),
      moved: false,
      pointerId: event.pointerId,
    };
    $('#plotArea').setPointerCapture(event.pointerId);
    return;
  }
  event.preventDefault();
  $('#plotArea').focus();
  const marker = event.target.closest('.marker');
  const handle = event.target.closest('[data-handle]')?.dataset.handle;
  const candidates = hitCandidates(point);
  const selection = Model.file(state).selection;
  let target = candidates.find(
    item =>
      item.annotation.id === selection.activeId &&
      (item.part.id === selection.segmentId || !item.part.id)
  );
  if (!target && candidates.length > 1 && !handle && !manualTool) {
    showModal(
      '选择重叠标记',
      '<p>此处有多个对象，请明确选择。</p><div class="segment-list">' +
        candidates
          .map(
            item =>
              /* HTML */ `<button
                data-overlap="${item.annotation.id}"
                data-overlap-segment="${item.part.id ?? ''}"
              >
                ${sourceNames[item.annotation.sourceKind]} ·
                ${escapeHtml(item.annotation.name)} ·
                ${item.part.id || '时间未定位'}
              </button>`
          )
          .join('') +
        '</div>',
      '<button data-action="close-modal">取消</button>',
      null,
      'overlap'
    );
    return;
  }
  target ??=
    candidates.find(
      item => item.annotation.id === marker?.dataset.annotation
    ) ?? candidates[0];
  if (target && !manualTool) {
    selectAnnotation(target.annotation.id, event, target.part.id);
    if (
      event.ctrlKey ||
      event.metaKey ||
      event.shiftKey ||
      !Model.file(state).selection.ids.includes(target.annotation.id)
    )
      return;
    gesture = {
      type: 'edit',
      id: target.annotation.id,
      segmentId: target.part.id,
      original: clone(target.part.range),
      revisionId: target.annotation.revisionId,
      start: point,
      handle,
      moved: false,
      preview: null,
    };
  } else
    gesture = {
      type: manualTool ? 'manual' : 'zoom-box',
      start: point,
      originalView: clone(Model.file(state).view),
      moved: false,
      preview: null,
    };
  gesture.pointerId = event.pointerId;
  $('#plotArea').setPointerCapture(event.pointerId);
});

$('#modalContent').addEventListener('click', event => {
  const button = event.target.closest('[data-overlap]');
  if (button) {
    const id = button.dataset.overlap;
    const segmentId = button.dataset.overlapSegment || null;
    closeModal();
    selectAnnotation(id, {}, segmentId);
  }
});

function movedRange(point) {
  const sourceFile = Model.file(state);
  const from = physicalPoint(gesture.start.x, gesture.start.y);
  const to = physicalPoint(point.x, point.y);
  const sampleDelta = Math.round(
    ((to.timeMs - from.timeMs) * sourceFile.sampleRateHz) / 1000
  );
  const frequencyDelta = (to.frequencyMHz - from.frequencyMHz) * 1e6;
  const range = clone(gesture.original);
  const handle = gesture.handle;
  const timeAxis = state.mode === 'waterfall' ? ['n', 's'] : ['w', 'e'];
  const frequencyAxis = state.mode === 'waterfall' ? ['w', 'e'] : ['s', 'n'];
  if (gesture.segmentId && !handle) {
    range.startSample += sampleDelta;
    range.endSample += sampleDelta;
  } else if (gesture.segmentId && handle?.includes(timeAxis[0]))
    range.startSample += sampleDelta;
  else if (gesture.segmentId && handle?.includes(timeAxis[1]))
    range.endSample += sampleDelta;
  if (!handle) {
    range.frequencyLowerHz += frequencyDelta;
    range.frequencyUpperHz += frequencyDelta;
  } else {
    if (handle.includes(frequencyAxis[0]))
      range.frequencyLowerHz += frequencyDelta;
    if (handle.includes(frequencyAxis[1]))
      range.frequencyUpperHz += frequencyDelta;
  }
  return range;
}

$('#plotArea').addEventListener('pointermove', event => {
  const point = pointerPosition(event);
  if (!gesture) {
    const marker = event.target.closest('.marker');
    setHover(
      marker?.dataset.annotation || null,
      marker?.dataset.segmentId || null
    );
    return;
  }
  if (
    Math.hypot(point.x - gesture.start.x, point.y - gesture.start.y) < 4 &&
    !gesture.moved
  )
    return;
  gesture.moved = true;
  if (gesture.type === 'edit') {
    gesture.preview = movedRange(point);
    drawMarkers();
  } else if (['manual', 'zoom-box'].includes(gesture.type)) {
    const from = physicalPoint(gesture.start.x, gesture.start.y);
    const to = physicalPoint(point.x, point.y);
    try {
      gesture.preview = Model.fromDisplayRange(Model.file(state), {
        startMs: Math.min(from.timeMs, to.timeMs),
        endMs: Math.max(from.timeMs, to.timeMs),
        lowerMHz: Math.min(from.frequencyMHz, to.frequencyMHz),
        upperMHz: Math.max(from.frequencyMHz, to.frequencyMHz),
      });
    } catch {
      gesture.preview = null;
    }
    drawMarkers();
  } else {
    const area = plotGeometry();
    const initial = gesture.originalView;
    const deltaX = gesture.axis === 'x' ? point.x - gesture.start.x : 0;
    const deltaY = gesture.axis === 'y' ? point.y - gesture.start.y : 0;
    const timePixel =
      state.mode === 'waterfall' ? deltaY / area.height : deltaX / area.width;
    const frequencyPixel =
      state.mode === 'waterfall' ? deltaX / area.width : -deltaY / area.height;
    const duration =
      (Model.file(state).totalSamples / Model.file(state).sampleRateHz) * 1000;
    const timeWidth = initial.endMs - initial.startMs;
    const frequencyWidth = initial.upperMHz - initial.lowerMHz;
    const lower =
      (Model.file(state).centerFrequencyHz -
        Model.file(state).sampleRateHz / 2) /
      1e6;
    const start = Math.min(
      duration - timeWidth,
      Math.max(0, initial.startMs - timePixel * timeWidth)
    );
    const frequency = Math.min(
      lower + Model.file(state).sampleRateHz / 1e6 - frequencyWidth,
      Math.max(lower, initial.lowerMHz - frequencyPixel * frequencyWidth)
    );
    Model.file(state).view = {
      startMs: start,
      endMs: start + timeWidth,
      lowerMHz: frequency,
      upperMHz: frequency + frequencyWidth,
    };
    drawPlots();
  }
});

$('#plotArea').addEventListener('pointerup', event => {
  if (!gesture) return;
  const pending = gesture;
  gesture = null;
  if ($('#plotArea').hasPointerCapture(event.pointerId))
    $('#plotArea').releasePointerCapture(event.pointerId);
  if (pending.type === 'edit' && pending.moved && pending.preview) {
    run(
      () =>
        Model.edit(
          state,
          pending.id,
          pending.segmentId
            ? { segmentId: pending.segmentId, range: pending.preview }
            : {
                frequency: {
                  lowerHz: pending.preview.frequencyLowerHz,
                  upperHz: pending.preview.frequencyUpperHz,
                },
              },
          pending.revisionId
        ),
      '图形范围已提交；一次拖动对应一次撤销。'
    );
  } else if (pending.type === 'manual' && pending.preview)
    run(
      () => {
        Model.createManual(state, pending.preview);
        Model.ensurePageForActive(state);
      },
      '已创建手动标记；可继续拖动创建。',
      { locate: true }
    );
  else if (pending.type === 'zoom-box' && pending.moved && pending.preview) {
    const display = Model.toDisplayRange(Model.file(state), pending.preview);
    Model.file(state).view = {
      startMs: display.startMs,
      endMs: display.endMs,
      lowerMHz: display.lowerMHz,
      upperMHz: display.upperMHz,
    };
  } else if (
    pending.type === 'zoom-box' &&
    !pending.moved &&
    !event.ctrlKey &&
    !event.shiftKey
  )
    run(() => Model.clearSelection(state));
  render();
});
$('#plotArea').addEventListener('pointercancel', () => {
  cancelGesture();
  render();
});
$('#plotArea').addEventListener('mouseleave', () => {
  if (!gesture) setHover();
});
$('#plotArea').addEventListener(
  'wheel',
  event => {
    event.preventDefault();
    if (draft || modal) return;
    const point = pointerPosition(event);
    const area = plotGeometry();
    const axis =
      point.x < area.left
        ? state.mode === 'waterfall'
          ? 'time'
          : 'frequency'
        : point.y > area.top + area.height
          ? state.mode === 'waterfall'
            ? 'frequency'
            : 'time'
          : null;
    zoomView(event.deltaY < 0 ? 0.8 : 1.25, axis);
  },
  { passive: false }
);
new ResizeObserver(() => drawPlots()).observe($('#plotArea'));

// Read-only introspection for black-box browser checks; no state mutation shortcuts.
window.annotationPrototype = Object.freeze({
  snapshot: () => ({
    ...clone(Model.snapshot(state)),
    undoCount: state.undo.length,
    redoCount: state.redo.length,
    draft: draft
      ? {
          id: draft.id,
          field: draft.field,
          text: $('[data-cell-editor]')?.value,
        }
      : null,
  }),
  geometry: () => ({
    ...plotGeometry(),
    markers: Model.visibleAnnotations(state).flatMap(annotation =>
      (annotation.segments.length
        ? annotation.segments
        : [{ id: null, range: annotation.observation }]
      ).map(part => ({
        id: annotation.id,
        segmentId: part.id,
        ...rangeRect(part.range),
      }))
    ),
  }),
});
render({ reveal: true });
