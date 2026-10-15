// 独立浏览器交互演示。所有夹具均为合成数据；本模块不读取 IQ，也不执行 DSP 或模型。
const SCHEMA = 'signal-studio-annotation-interaction-prototype';
const SOURCES = ['ImportedLabel', 'Manual', 'Algorithm'];
const QUALITIES = ['Pending', 'Accepted', 'Rejected'];
const SCENARIOS = [
  'standard',
  'large',
  'partial',
  'outside',
  'changed-source',
  'missing-source',
];
const SORT_KEYS = [
  'id',
  'name',
  'source',
  'sourceKind',
  'quality',
  'reviewStatus',
  'score',
  'segments',
  'startMs',
  'endMs',
  'centerMHz',
  'bandwidthKHz',
  'startSample',
  'endSample',
  'frequencyLowerHz',
  'frequencyUpperHz',
  'revisionNumber',
];
const RANGE_KEYS = [
  'startSample',
  'endSample',
  'frequencyLowerHz',
  'frequencyUpperHz',
];
let revisionToken = 0;

function copy(value) {
  return JSON.parse(JSON.stringify(value));
}

function fail(message) {
  throw new Error(message);
}

function isRecord(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function finite(value) {
  return typeof value === 'number' && Number.isFinite(value);
}

function integer(value) {
  return Number.isSafeInteger(value);
}

function freeze(value) {
  if (value && typeof value === 'object') {
    Object.values(value).forEach(freeze);
    Object.freeze(value);
  }
  return value;
}

function freezeOrigins(state) {
  for (const sourceFile of state.files) {
    for (const annotation of sourceFile.annotations) freeze(annotation.origin);
    for (const channel of sourceFile.channels) freeze(channel.sourceSnapshot);
  }
}

function sameRange(left, right) {
  return Boolean(
    left && right && RANGE_KEYS.every(key => left[key] === right[key])
  );
}

function rangeInside(inner, outer) {
  return (
    inner.startSample >= outer.startSample &&
    inner.endSample <= outer.endSample &&
    inner.frequencyLowerHz >= outer.frequencyLowerHz &&
    inner.frequencyUpperHz <= outer.frequencyUpperHz
  );
}

function checkedRange(
  sourceFile,
  input,
  label = '标记范围',
  { allowEmpty = false } = {}
) {
  if (!isRecord(input)) fail(`${label}必须包含样本起止和频率上下界。`);
  const result = Object.fromEntries(RANGE_KEYS.map(key => [key, input[key]]));
  if (!integer(result.startSample) || !integer(result.endSample)) {
    fail(`${label}的样本起止必须为安全整数。`);
  }
  if (
    result.startSample < 0 ||
    result.endSample > sourceFile.totalSamples ||
    result.startSample > result.endSample ||
    (!allowEmpty && result.startSample === result.endSample)
  ) {
    fail(`${label}必须位于文件物理样本范围内，且起点小于终点。`);
  }
  if (
    !finite(result.frequencyLowerHz) ||
    !finite(result.frequencyUpperHz) ||
    result.frequencyLowerHz >= result.frequencyUpperHz
  ) {
    fail(`${label}的频率下界必须小于频率上界。`);
  }
  const lower = sourceFile.centerFrequencyHz - sourceFile.sampleRateHz / 2;
  const upper = sourceFile.centerFrequencyHz + sourceFile.sampleRateHz / 2;
  if (result.frequencyLowerHz < lower || result.frequencyUpperHz > upper) {
    fail(`${label}的频率必须位于文件采样频带内。`);
  }
  return result;
}

function selection() {
  return {
    ids: [],
    activeId: null,
    segmentId: null,
    anchorId: null,
    hoverId: null,
    hoverSegmentId: null,
  };
}

function makeRange(sourceFile, startMs, endMs, lowerMHz, upperMHz) {
  return {
    startSample: Math.round((startMs * sourceFile.sampleRateHz) / 1000),
    endSample: Math.round((endMs * sourceFile.sampleRateHz) / 1000),
    frequencyLowerHz: lowerMHz * 1e6,
    frequencyUpperHz: upperMHz * 1e6,
  };
}

function makeSegment(id, range, sourceKind) {
  return {
    id,
    range: copy(range),
    timeQuality:
      sourceKind === 'Algorithm'
        ? 'SyntheticEstimated'
        : sourceKind === 'ImportedLabel'
          ? 'SampleLabel'
          : 'Manual',
    frequencyQuality:
      sourceKind === 'ImportedLabel' ? 'NominalLabel' : 'Synthetic',
    confirmed: false,
  };
}

function makeAnnotation(
  sourceFile,
  id,
  name,
  sourceKind,
  segments,
  observation = null,
  score = null,
  reviewStatus = 'Pending'
) {
  return {
    id,
    name,
    note: '',
    sourceKind,
    reviewStatus,
    revisionId: `${id}-r0`,
    revisionNumber: 0,
    origin: freeze({
      name,
      segments: copy(segments),
      observation: copy(observation),
      evidence: {
        demoOnly: true,
        description:
          sourceKind === 'ImportedLabel'
            ? '内嵌四条标签的几何演示；未读取原始 IQ。'
            : '独立合成浏览器交互夹具；未执行模型或 DSP。',
      },
    }),
    segments,
    activeSegmentId: segments[0]?.id ?? null,
    observation,
    score,
    deleted: false,
    revisionHistory: [],
    qualityHistory: [],
    sourceIdentity: sourceFile.sourceIdentity,
  };
}

function makeFile(id, name, sampleRateHz, totalSamples) {
  return {
    id,
    name,
    sampleRateHz,
    centerFrequencyHz: 2.5e9,
    totalSamples,
    availableSamples: totalSamples,
    sourceAvailable: true,
    sourceIdentity: `demo-${id}-v1`,
    view: {
      startMs: 0,
      endMs: (totalSamples / sampleRateHz) * 1000,
      lowerMHz: (2.5e9 - sampleRateHz / 2) / 1e6,
      upperMHz: (2.5e9 + sampleRateHz / 2) / 1e6,
    },
    annotations: [],
    channels: [],
    selection: selection(),
  };
}

export function createState(scenario = 'standard') {
  if (!SCENARIOS.includes(scenario)) fail(`不支持的演示场景：${scenario}。`);
  const first = makeFile('file-a', '20 MHz · 三来源统一标记演示', 20e6, 400000);
  const second = makeFile('file-b', '60 MHz · 空文件演示', 60e6, 600000);
  // 几何常量内嵌在本模块，运行时不依赖旧原型或外部数据。
  const labels = [
    [51107, 292170, 2502021605.9327126, 2502282009.1227803, '64qam'],
    [38498, 315326, 2492059313.3481555, 2492749655.595414, '2gmsk'],
    [70248, 289733, 2491820030.553922, 2492454138.891255, 'bpsk'],
    [173404, 171890, 2502885255.8388267, 2503155805.4215245, '64qam'],
  ];
  first.annotations = labels.map((row, index) => {
    const id = `label-${index + 1}`;
    const range = {
      startSample: row[0],
      endSample: row[0] + row[1],
      frequencyLowerHz: row[2],
      frequencyUpperHz: row[3],
    };
    return makeAnnotation(
      first,
      id,
      `GT 0${index + 1} · ${row[4]}`,
      'ImportedLabel',
      [makeSegment(`${id}-s1`, range, 'ImportedLabel')]
    );
  });
  const manual = makeAnnotation(
    first,
    'manual-1',
    'Manual 01 · 双突发复核区',
    'Manual',
    [
      makeSegment(
        'manual-1-s1',
        makeRange(first, 4, 6, 2496.8, 2497.55),
        'Manual'
      ),
      makeSegment(
        'manual-1-s2',
        makeRange(first, 9, 12, 2496.8, 2497.55),
        'Manual'
      ),
    ],
    null,
    null,
    'Accepted'
  );
  const located = makeAnnotation(
    first,
    'algo-located',
    'ALG 01 · 已定位双活动段',
    'Algorithm',
    [
      makeSegment(
        'algo-located-s1',
        makeRange(first, 2.9, 7.1, 2502, 2502.34),
        'Algorithm'
      ),
      makeSegment(
        'algo-located-s2',
        makeRange(first, 9.4, 16.6, 2502, 2502.34),
        'Algorithm'
      ),
    ],
    null,
    0.94
  );
  const observation = makeAnnotation(
    first,
    'algo-observation',
    'ALG 02 · 时间未定位观察带',
    'Algorithm',
    [],
    makeRange(first, 0, 13.2768, 2499.38, 2500.12),
    0.82
  );
  const ignored = makeAnnotation(
    first,
    'algo-ignored',
    'ALG 03 · 已忽略演示对象',
    'Algorithm',
    [
      makeSegment(
        'algo-ignored-s1',
        makeRange(first, 5, 15, 2505.1, 2505.8),
        'Algorithm'
      ),
    ],
    null,
    0.73,
    'Rejected'
  );
  first.annotations.push(manual, located, observation, ignored);
  const state = {
    schema: SCHEMA,
    version: 6,
    demoOnly: true,
    scenario,
    files: [first, second],
    activeFileId: first.id,
    filters: {
      source: 'all',
      quality: 'all',
      score: 0,
      layers: { ImportedLabel: true, Manual: true, Algorithm: true },
    },
    sort: { key: 'name', ascending: true },
    page: 0,
    pageSize: 200,
    treeLimit: { ImportedLabel: 40, Manual: 40, Algorithm: 40 },
    mode: 'spectrogram',
    undo: [],
    redo: [],
  };
  select(state, located.id);
  const initialRequest = defaultChannelRequest(state);
  first.channels.push(
    channelFromRequest(first, located, 'channel-1', {
      ...initialRequest,
      name: '演示通道 · 已定位第 1 段',
    })
  );
  select(state, observation.id);
  if (scenario === 'large') {
    for (let index = 1; index <= 260; index += 1) {
      const id = `bulk-${String(index).padStart(3, '0')}`;
      const kind = 'Algorithm';
      const start = 0.3 + (index % 25) * 0.65;
      const lower = 2490.6 + (index % 17) * 1.02;
      first.annotations.push(
        makeAnnotation(
          first,
          id,
          `批量 ${String(index).padStart(3, '0')}`,
          kind,
          [
            makeSegment(
              `${id}-s1`,
              makeRange(first, start, start + 0.8, lower, lower + 0.16),
              kind
            ),
          ],
          null,
          0.5 + (index % 50) / 100,
          QUALITIES[(index - 1) % QUALITIES.length]
        )
      );
    }
  }
  if (['partial', 'outside'].includes(scenario))
    first.availableSamples = 240000;
  if (scenario === 'outside')
    first.view = { ...first.view, startMs: 15, endMs: 20 };
  if (scenario === 'changed-source') first.sourceIdentity = 'demo-file-a-v2';
  if (scenario === 'missing-source') first.sourceAvailable = false;
  return state;
}

export function file(state) {
  return (
    state.files.find(sourceFile => sourceFile.id === state.activeFileId) ?? null
  );
}

export function active(state) {
  const sourceFile = file(state);
  return (
    sourceFile?.annotations.find(
      annotation =>
        annotation.id === sourceFile.selection.activeId && !annotation.deleted
    ) ?? null
  );
}

export function segment(state, annotation = active(state)) {
  if (!annotation || annotation.deleted) return null;
  return (
    annotation.segments.find(part => part.id === annotation.activeSegmentId) ??
    null
  );
}

export function toDisplayRange(sourceFile, range) {
  if (
    !sourceFile ||
    !isRecord(range) ||
    !RANGE_KEYS.every(key => finite(range[key]))
  ) {
    fail('显示范围需要文件参数及有限的物理样本和频率值。');
  }
  return {
    startMs: (range.startSample / sourceFile.sampleRateHz) * 1000,
    endMs: (range.endSample / sourceFile.sampleRateHz) * 1000,
    centerMHz: (range.frequencyLowerHz + range.frequencyUpperHz) / 2e6,
    bandwidthKHz: (range.frequencyUpperHz - range.frequencyLowerHz) / 1000,
    lowerMHz: range.frequencyLowerHz / 1e6,
    upperMHz: range.frequencyUpperHz / 1e6,
  };
}

export function fromDisplayRange(sourceFile, display) {
  if (
    !sourceFile ||
    !isRecord(display) ||
    !['startMs', 'endMs', 'lowerMHz', 'upperMHz'].every(key =>
      finite(display[key])
    )
  ) {
    fail('输入范围需要有限的毫秒起止和 MHz 频率上下界。');
  }
  return checkedRange(
    sourceFile,
    makeRange(
      sourceFile,
      display.startMs,
      display.endMs,
      display.lowerMHz,
      display.upperMHz
    )
  );
}

function sortValue(sourceFile, annotation, key) {
  const currentRange =
    annotation.segments.find(part => part.id === annotation.activeSegmentId)
      ?.range ?? null;
  const frequencyRange =
    currentRange ??
    (annotation.segments.length ? null : annotation.observation);
  if (key === 'source') return annotation.sourceKind;
  if (key === 'quality') return annotation.reviewStatus;
  if (key === 'segments') return annotation.segments.length;
  // 观察支持并不是活动起止；主表的当前段与排序必须使用同一物理范围。
  if (['startMs', 'endMs'].includes(key)) {
    return currentRange ? toDisplayRange(sourceFile, currentRange)[key] : null;
  }
  if (['startSample', 'endSample'].includes(key))
    return currentRange?.[key] ?? null;
  if (['centerMHz', 'bandwidthKHz'].includes(key)) {
    return frequencyRange
      ? toDisplayRange(sourceFile, frequencyRange)[key]
      : null;
  }
  if (['frequencyLowerHz', 'frequencyUpperHz'].includes(key))
    return frequencyRange?.[key] ?? null;
  return annotation[key];
}

function annotationsForFile(state, sourceFile) {
  if (!sourceFile) return [];
  const { source, quality, score, layers } = state.filters;
  return sourceFile.annotations
    .filter(
      annotation =>
        !annotation.deleted &&
        layers[annotation.sourceKind] &&
        (source === 'all' || annotation.sourceKind === source) &&
        (quality === 'all' || annotation.reviewStatus === quality) &&
        (score === 0 ||
          (annotation.score !== null && annotation.score >= score))
    )
    .sort((left, right) => {
      const a = sortValue(sourceFile, left, state.sort.key);
      const b = sortValue(sourceFile, right, state.sort.key);
      const aUnknown = a === null || a === undefined;
      const bUnknown = b === null || b === undefined;
      if (aUnknown || bUnknown) {
        return aUnknown === bUnknown
          ? left.id.localeCompare(right.id)
          : aUnknown
            ? 1
            : -1;
      }
      let order;
      if (typeof a === 'string')
        order = a.localeCompare(String(b), 'zh-CN', { numeric: true });
      else order = a < b ? -1 : a > b ? 1 : 0;
      return (
        (state.sort.ascending ? order : -order) ||
        left.id.localeCompare(right.id)
      );
    });
}

export function visibleAnnotations(state) {
  return annotationsForFile(state, file(state));
}

function syncActiveSegment(sourceFile) {
  const current = sourceFile.annotations.find(
    annotation =>
      !annotation.deleted && annotation.id === sourceFile.selection.activeId
  );
  if (!current) {
    sourceFile.selection.segmentId = null;
    return;
  }
  const chosen =
    current.segments.find(part => part.id === sourceFile.selection.segmentId) ??
    current.segments.find(part => part.id === current.activeSegmentId) ??
    current.segments[0] ??
    null;
  current.activeSegmentId = chosen?.id ?? null;
  sourceFile.selection.segmentId = chosen?.id ?? null;
}

function reconcileFileSelection(state, sourceFile) {
  if (!sourceFile) fail('当前文件不存在，无法整理选择。');
  const visible = annotationsForFile(state, sourceFile);
  const visibleIds = new Set(visible.map(annotation => annotation.id));
  const selected = new Set(
    sourceFile.selection.ids.filter(id => visibleIds.has(id))
  );
  sourceFile.selection.ids = visible
    .filter(annotation => selected.has(annotation.id))
    .map(annotation => annotation.id);
  if (!selected.has(sourceFile.selection.activeId)) {
    sourceFile.selection.activeId = sourceFile.selection.ids.at(-1) ?? null;
    sourceFile.selection.segmentId = null;
  }
  if (!visibleIds.has(sourceFile.selection.anchorId)) {
    sourceFile.selection.anchorId = sourceFile.selection.activeId;
  }
  const hovered = visible.find(
    annotation => annotation.id === sourceFile.selection.hoverId
  );
  if (!hovered) {
    sourceFile.selection.hoverId = null;
    sourceFile.selection.hoverSegmentId = null;
  } else if (
    !hovered.segments.some(
      part => part.id === sourceFile.selection.hoverSegmentId
    )
  ) {
    sourceFile.selection.hoverSegmentId = null;
  }
  syncActiveSegment(sourceFile);
  if (sourceFile.id === state.activeFileId) {
    const lastPage = Math.max(
      0,
      Math.ceil(visible.length / state.pageSize) - 1
    );
    state.page = Math.min(Math.max(0, state.page), lastPage);
  }
  return sourceFile.selection;
}

export function reconcileSelection(state) {
  return reconcileFileSelection(state, file(state));
}

export function ensurePageForActive(state) {
  reconcileSelection(state);
  const index = visibleAnnotations(state).findIndex(
    annotation => annotation.id === active(state)?.id
  );
  if (index >= 0) state.page = Math.floor(index / state.pageSize);
  return state.page;
}

export function select(
  state,
  id,
  { ctrlKey = false, shiftKey = false, orderedIds, segmentId } = {}
) {
  const sourceFile = file(state);
  const visible = visibleAnnotations(state);
  const target = visible.find(annotation => annotation.id === id);
  if (!target) fail(`对象 ${id} 不在当前文件的可见结果中。`);
  if (
    segmentId !== undefined &&
    segmentId !== null &&
    !target.segments.some(part => part.id === segmentId)
  ) {
    fail(`对象 ${id} 不包含活动段 ${segmentId}。`);
  }
  const visibleIds = new Set(visible.map(annotation => annotation.id));
  const order = orderedIds
    ? [...new Set(orderedIds.filter(value => visibleIds.has(value)))]
    : visible.map(annotation => annotation.id);
  if (!order.includes(id)) fail(`选择顺序中缺少对象 ${id}。`);
  const current = sourceFile.selection;
  if (shiftKey && current.anchorId && order.includes(current.anchorId)) {
    const start = order.indexOf(current.anchorId);
    const end = order.indexOf(id);
    const block = order.slice(Math.min(start, end), Math.max(start, end) + 1);
    current.ids = ctrlKey ? [...new Set([...current.ids, ...block])] : block;
    current.activeId = id;
  } else if (ctrlKey) {
    if (current.ids.includes(id)) {
      current.ids = current.ids.filter(value => value !== id);
      current.activeId =
        current.activeId === id
          ? (current.ids.at(-1) ?? null)
          : current.activeId;
    } else {
      current.ids.push(id);
      current.activeId = id;
    }
    current.anchorId = id;
  } else {
    current.ids = [id];
    current.activeId = id;
    current.anchorId = id;
  }
  if (current.activeId === id)
    current.segmentId = segmentId ?? target.activeSegmentId;
  else current.segmentId = null;
  reconcileSelection(state);
  ensurePageForActive(state);
  return target;
}

export function clearSelection(state) {
  const current = file(state).selection;
  current.ids = [];
  current.activeId = null;
  current.segmentId = null;
  current.anchorId = null;
  return current;
}

export function selectAll(state) {
  const visible = visibleAnnotations(state);
  const current = file(state).selection;
  current.ids = visible.map(annotation => annotation.id);
  current.activeId = current.ids.includes(current.activeId)
    ? current.activeId
    : (current.ids[0] ?? null);
  current.anchorId = current.activeId;
  current.segmentId = null;
  return reconcileSelection(state);
}

function checkedFilters(input) {
  if (
    !isRecord(input) ||
    !['all', ...SOURCES].includes(input.source) ||
    !['all', ...QUALITIES].includes(input.quality) ||
    !finite(input.score) ||
    input.score < 0 ||
    input.score > 1 ||
    !isRecord(input.layers) ||
    !SOURCES.every(key => typeof input.layers[key] === 'boolean')
  ) {
    fail('筛选条件无效：来源、复核状态、0–1 评分和三来源图层开关均须合法。');
  }
  return input;
}

export function applyFilter(state, patch) {
  if (
    !isRecord(patch) ||
    Object.keys(patch).some(
      key => !['source', 'quality', 'score', 'layers'].includes(key)
    )
  )
    fail('筛选补丁包含不支持的字段。');
  if (
    patch.layers !== undefined &&
    (!isRecord(patch.layers) ||
      Object.entries(patch.layers).some(
        ([key, value]) => !SOURCES.includes(key) || typeof value !== 'boolean'
      ))
  )
    fail('图层补丁必须使用三来源名称和布尔开关。');
  const next = {
    ...state.filters,
    ...patch,
    layers: { ...state.filters.layers, ...(patch.layers ?? {}) },
  };
  checkedFilters(next);
  state.filters = next;
  state.page = 0;
  reconcileSelection(state);
  return state.filters;
}

export function sortBy(state, key) {
  if (!SORT_KEYS.includes(key)) fail(`不支持按 ${key} 排序。`);
  state.sort = {
    key,
    ascending: state.sort.key === key ? !state.sort.ascending : true,
  };
  reconcileSelection(state);
  ensurePageForActive(state);
  return state.sort;
}

export function switchFile(state, id) {
  const target = state.files.find(sourceFile => sourceFile.id === id);
  if (!target) fail(`文件 ${id} 不存在。`);
  state.activeFileId = id;
  state.page = 0;
  target.selection.hoverId = null;
  target.selection.hoverSegmentId = null;
  reconcileSelection(state);
  ensurePageForActive(state);
  return target;
}

function annotationById(state, id) {
  const result = file(state)?.annotations.find(
    annotation => annotation.id === id && !annotation.deleted
  );
  if (!result) fail(`当前文件中找不到未删除对象 ${id}。`);
  return result;
}

function partById(annotation, id) {
  const result = annotation.segments.find(part => part.id === id);
  if (!result) fail(`对象 ${annotation.id} 不包含活动段 ${id}。`);
  return result;
}

function selectionSnapshot(sourceFile) {
  const { ids, activeId, segmentId, anchorId } = sourceFile.selection;
  return { ids: [...ids], activeId, segmentId, anchorId };
}

function recordSnapshot(state) {
  return state.files.map(sourceFile => ({
    fileId: sourceFile.id,
    annotations: copy(sourceFile.annotations),
    channels: copy(sourceFile.channels),
    selection: selectionSnapshot(sourceFile),
  }));
}

function recordDifferences(before, after) {
  const prior = new Map(
    before.map((record, index) => [record.id, { record, index }])
  );
  const next = new Map(
    after.map((record, index) => [record.id, { record, index }])
  );
  const changes = [];
  for (const id of new Set([...prior.keys(), ...next.keys()])) {
    const oldValue = prior.get(id)?.record ?? null;
    const newValue = next.get(id)?.record ?? null;
    if (JSON.stringify(oldValue) !== JSON.stringify(newValue)) {
      changes.push({
        id,
        before: copy(oldValue),
        after: copy(newValue),
        beforeIndex: prior.get(id)?.index ?? null,
        afterIndex: next.get(id)?.index ?? null,
      });
    }
  }
  return changes;
}

function commandDifferences(before, after) {
  const changes = [];
  for (const oldFile of before) {
    const newFile = after.find(
      sourceFile => sourceFile.fileId === oldFile.fileId
    );
    if (!newFile) continue;
    const annotationChanges = recordDifferences(
      oldFile.annotations,
      newFile.annotations
    );
    const channelChanges = recordDifferences(
      oldFile.channels,
      newFile.channels
    );
    if (annotationChanges.length || channelChanges.length) {
      changes.push({
        fileId: oldFile.fileId,
        annotationChanges,
        channelChanges,
        beforeSelection: oldFile.selection,
        afterSelection: newFile.selection,
      });
    }
  }
  return changes;
}

function applyRecordDifferences(records, changes, direction) {
  for (const change of changes) {
    const desired = change[direction];
    const currentIndex = records.findIndex(record => record.id === change.id);
    if (desired === null) {
      if (currentIndex >= 0) records.splice(currentIndex, 1);
      continue;
    }
    const replacement = copy(desired);
    if (replacement.origin) freeze(replacement.origin);
    if (replacement.sourceSnapshot) freeze(replacement.sourceSnapshot);
    if (currentIndex >= 0) records[currentIndex] = replacement;
    else
      records.splice(
        Math.min(change[`${direction}Index`], records.length),
        0,
        replacement
      );
  }
}

function applyCommandDifferences(state, changes, direction) {
  if (
    changes.some(
      change => !state.files.some(sourceFile => sourceFile.id === change.fileId)
    )
  ) {
    fail('撤销或重做涉及的文件已不存在，无法应用记录差异。');
  }
  for (const change of changes) {
    const sourceFile = state.files.find(value => value.id === change.fileId);
    const expectedSelection =
      direction === 'before' ? change.afterSelection : change.beforeSelection;
    const targetSelection =
      direction === 'before' ? change.beforeSelection : change.afterSelection;
    const selectionUnchanged =
      JSON.stringify(selectionSnapshot(sourceFile)) ===
      JSON.stringify(expectedSelection);
    applyRecordDifferences(
      sourceFile.annotations,
      change.annotationChanges,
      direction
    );
    applyRecordDifferences(
      sourceFile.channels,
      change.channelChanges,
      direction
    );
    // 仅恢复命令自己改变的选择；期间选择了新发布对象时保留用户当前选择。
    if (selectionUnchanged)
      Object.assign(sourceFile.selection, copy(targetSelection));
    reconcileFileSelection(state, sourceFile);
  }
}

function command(state, label, update) {
  const before = recordSnapshot(state);
  let result;
  try {
    result = update();
    reconcileSelection(state);
  } catch (error) {
    const partialChanges = commandDifferences(before, recordSnapshot(state));
    applyCommandDifferences(state, partialChanges, 'before');
    throw error;
  }
  const changes = commandDifferences(before, recordSnapshot(state));
  if (changes.length) {
    state.undo.push({ label, changes });
    state.redo = [];
  }
  return result;
}

function recordRevision(annotation, kind, details, geometryChanged = false) {
  const previousRevisionId = annotation.revisionId;
  const previousStatus = annotation.reviewStatus;
  annotation.revisionNumber += 1;
  revisionToken += 1;
  annotation.revisionId = `${annotation.id}-r${annotation.revisionNumber}-${revisionToken}`;
  annotation.revisionHistory.push({
    previousRevisionId,
    revisionId: annotation.revisionId,
    revisionNumber: annotation.revisionNumber,
    kind,
    details: copy(details),
    segments: copy(annotation.segments),
    observation: copy(annotation.observation),
    at: 'DEMO',
  });
  if (geometryChanged) {
    annotation.reviewStatus = 'Pending';
    annotation.qualityHistory.push({
      from: previousStatus,
      to: 'Pending',
      revisionId: annotation.revisionId,
      reason: '几何或活动段结构已修改',
      at: 'DEMO',
    });
  }
}

function setActivePart(sourceFile, annotation, id) {
  annotation.activeSegmentId = id;
  if (sourceFile.selection.activeId === annotation.id)
    sourceFile.selection.segmentId = id;
}

function sortSegments(annotation) {
  annotation.segments.sort(
    (left, right) =>
      left.range.startSample - right.range.startSample ||
      left.range.endSample - right.range.endSample ||
      left.id.localeCompare(right.id)
  );
}

export function edit(state, id, patch, baseRevisionId) {
  const sourceFile = file(state);
  const annotation = annotationById(state, id);
  if (
    baseRevisionId !== undefined &&
    baseRevisionId !== annotation.revisionId
  ) {
    fail('编辑依据的修订已过期；请重新载入当前对象后再提交。');
  }
  if (
    !isRecord(patch) ||
    Object.keys(patch).some(
      key => !['name', 'note', 'segmentId', 'range', 'frequency'].includes(key)
    )
  )
    fail('编辑补丁包含不支持的字段。');
  if (
    patch.name !== undefined &&
    (typeof patch.name !== 'string' || !patch.name.trim())
  ) {
    fail('对象名称不能为空。');
  }
  if (patch.note !== undefined && typeof patch.note !== 'string')
    fail('对象备注必须为文本。');
  if (patch.range !== undefined && patch.frequency !== undefined) {
    fail('一次编辑不能同时提交活动段范围和无段观察带频率。');
  }
  const targetId = patch.segmentId ?? annotation.activeSegmentId;
  const target =
    patch.range !== undefined ? partById(annotation, targetId) : null;
  if (patch.segmentId !== undefined && patch.segmentId !== null)
    partById(annotation, patch.segmentId);
  const nextRange =
    patch.range === undefined ? null : checkedRange(sourceFile, patch.range);
  let nextObservation = null;
  if (patch.frequency !== undefined) {
    if (annotation.segments.length || !annotation.observation) {
      fail('仅时间未定位、没有活动段的对象可以编辑观察带频率。');
    }
    if (!isRecord(patch.frequency))
      fail('观察带频率必须包含 lowerHz 与 upperHz。');
    nextObservation = checkedRange(sourceFile, {
      ...annotation.observation,
      frequencyLowerHz: patch.frequency.lowerHz,
      frequencyUpperHz: patch.frequency.upperHz,
    });
  }
  const geometryChanged = Boolean(
    (nextRange && !sameRange(target.range, nextRange)) ||
      (nextObservation && !sameRange(annotation.observation, nextObservation))
  );
  const metadataChanged =
    (patch.name !== undefined && patch.name.trim() !== annotation.name) ||
    (patch.note !== undefined && patch.note !== annotation.note);
  if (!geometryChanged && !metadataChanged) return annotation;
  return command(
    state,
    geometryChanged ? '编辑物理范围' : '编辑名称与备注',
    () => {
      if (patch.name !== undefined) annotation.name = patch.name.trim();
      if (patch.note !== undefined) annotation.note = patch.note;
      if (nextRange) {
        target.range = nextRange;
        sortSegments(annotation);
        setActivePart(sourceFile, annotation, target.id);
      }
      if (nextObservation) annotation.observation = nextObservation;
      recordRevision(
        annotation,
        geometryChanged ? 'Geometry' : 'Metadata',
        patch,
        geometryChanged
      );
      return annotation;
    }
  );
}

export function restoreOriginRange(state, id) {
  const sourceFile = file(state);
  const annotation = annotationById(state, id);
  const originSegments = copy(annotation.origin.segments);
  const originObservation = copy(annotation.origin.observation);
  if (
    JSON.stringify(annotation.segments) === JSON.stringify(originSegments) &&
    JSON.stringify(annotation.observation) === JSON.stringify(originObservation)
  )
    return annotation;
  return command(state, '恢复原始范围', () => {
    annotation.segments = originSegments;
    annotation.observation = originObservation;
    const previousId = annotation.activeSegmentId;
    setActivePart(
      sourceFile,
      annotation,
      annotation.segments.some(part => part.id === previousId)
        ? previousId
        : (annotation.segments[0]?.id ?? null)
    );
    recordRevision(annotation, 'RestoreOrigin', {}, true);
    return annotation;
  });
}

function nextSegmentId(sourceFile, annotation) {
  const reserved = new Set(
    [
      ...annotation.segments,
      ...annotation.origin.segments,
      ...annotation.revisionHistory.flatMap(
        revision => revision.segments ?? []
      ),
    ].map(part => part.id)
  );
  sourceFile.channels
    .filter(channel => channel.annotationId === annotation.id)
    .forEach(channel => reserved.add(channel.segmentId));
  let ordinal = 1;
  while (reserved.has(`${annotation.id}-s${ordinal}`)) ordinal += 1;
  return `${annotation.id}-s${ordinal}`;
}

export function addSegment(state, id, range) {
  const sourceFile = file(state);
  const annotation = annotationById(state, id);
  const nextRange = checkedRange(sourceFile, range);
  const added = makeSegment(
    nextSegmentId(sourceFile, annotation),
    nextRange,
    'Manual'
  );
  return command(state, '添加活动段', () => {
    annotation.segments.push(added);
    sortSegments(annotation);
    setActivePart(sourceFile, annotation, added.id);
    recordRevision(annotation, 'AddSegment', { segmentId: added.id }, true);
    return added;
  });
}

export function splitSegment(state, id, segmentId) {
  const sourceFile = file(state);
  const annotation = annotationById(state, id);
  const target = partById(annotation, segmentId);
  const midpoint =
    target.range.startSample +
    Math.floor((target.range.endSample - target.range.startSample) / 2);
  if (
    midpoint <= target.range.startSample ||
    midpoint >= target.range.endSample
  ) {
    fail('活动段不足两个样本，无法在样本中点拆分。');
  }
  const added = {
    ...copy(target),
    id: nextSegmentId(sourceFile, annotation),
    range: { ...target.range, startSample: midpoint },
  };
  return command(state, '拆分活动段', () => {
    target.range = { ...target.range, endSample: midpoint };
    annotation.segments.push(added);
    sortSegments(annotation);
    setActivePart(sourceFile, annotation, target.id);
    recordRevision(
      annotation,
      'SplitSegment',
      { segmentId, addedSegmentId: added.id },
      true
    );
    return [target, added];
  });
}

export function mergeAdjacent(state, id, segmentId) {
  const sourceFile = file(state);
  const annotation = annotationById(state, id);
  const target = partById(annotation, segmentId);
  const ordered = [...annotation.segments].sort(
    (left, right) =>
      left.range.startSample - right.range.startSample ||
      left.id.localeCompare(right.id)
  );
  if (ordered.length < 2) fail('对象只有一个活动段，没有可合并的相邻段。');
  const index = ordered.findIndex(part => part.id === segmentId);
  const adjacent = ordered[index + 1] ?? ordered[index - 1];
  const merged = {
    startSample: Math.min(target.range.startSample, adjacent.range.startSample),
    endSample: Math.max(target.range.endSample, adjacent.range.endSample),
    frequencyLowerHz: Math.min(
      target.range.frequencyLowerHz,
      adjacent.range.frequencyLowerHz
    ),
    frequencyUpperHz: Math.max(
      target.range.frequencyUpperHz,
      adjacent.range.frequencyUpperHz
    ),
  };
  return command(state, '合并相邻活动段', () => {
    target.range = merged;
    annotation.segments = annotation.segments.filter(
      part => part.id !== adjacent.id
    );
    sortSegments(annotation);
    setActivePart(sourceFile, annotation, target.id);
    recordRevision(
      annotation,
      'MergeSegments',
      { segmentId, removedSegmentId: adjacent.id },
      true
    );
    return target;
  });
}

export function deleteSegment(state, id, segmentId) {
  const sourceFile = file(state);
  const annotation = annotationById(state, id);
  partById(annotation, segmentId);
  return command(
    state,
    annotation.segments.length === 1 ? '删除最后活动段及对象' : '删除活动段',
    () => {
      if (annotation.segments.length === 1) {
        annotation.deleted = true;
        recordRevision(
          annotation,
          'SoftDelete',
          { lastSegmentId: segmentId },
          true
        );
      } else {
        annotation.segments = annotation.segments.filter(
          part => part.id !== segmentId
        );
        setActivePart(
          sourceFile,
          annotation,
          annotation.segments[0]?.id ?? null
        );
        recordRevision(annotation, 'DeleteSegment', { segmentId }, true);
      }
      return annotation;
    }
  );
}

export function createManual(state, range) {
  const sourceFile = file(state);
  const nextRange = checkedRange(sourceFile, range);
  let ordinal = 1;
  while (
    sourceFile.annotations.some(
      annotation => annotation.id === `manual-${ordinal}`
    )
  )
    ordinal += 1;
  const id = `manual-${ordinal}`;
  const created = makeAnnotation(
    sourceFile,
    id,
    `Manual ${String(ordinal).padStart(2, '0')} · 人工标记`,
    'Manual',
    [makeSegment(`${id}-s1`, nextRange, 'Manual')]
  );
  return command(state, '新建人工标记', () => {
    sourceFile.annotations.push(created);
    sourceFile.selection = {
      ...selection(),
      ids: [id],
      activeId: id,
      segmentId: created.activeSegmentId,
      anchorId: id,
    };
    return created;
  });
}

export function setQuality(state, status) {
  if (!QUALITIES.includes(status)) fail(`不支持的复核状态：${status}。`);
  reconcileSelection(state);
  const sourceFile = file(state);
  const selected = sourceFile.annotations.filter(
    annotation =>
      sourceFile.selection.ids.includes(annotation.id) &&
      annotation.reviewStatus !== status
  );
  if (!selected.length) return [];
  return command(state, `批量复核为 ${status}`, () => {
    selected.forEach(annotation => {
      annotation.qualityHistory.push({
        from: annotation.reviewStatus,
        to: status,
        revisionId: annotation.revisionId,
        reason: '用户复核',
        at: 'DEMO',
      });
      annotation.reviewStatus = status;
    });
    return selected;
  });
}

export function deleteSummary(state) {
  const sourceFile = file(state);
  const selectedIds = new Set(sourceFile.selection.ids);
  const annotations = sourceFile.annotations.filter(
    annotation => !annotation.deleted && selectedIds.has(annotation.id)
  );
  const selected = new Set(annotations.map(annotation => annotation.id));
  return {
    objects: annotations.length,
    segments: annotations.reduce(
      (sum, annotation) => sum + annotation.segments.length,
      0
    ),
    channels: sourceFile.channels.filter(channel =>
      selected.has(channel.annotationId)
    ).length,
  };
}

export function deleteSelected(state) {
  reconcileSelection(state);
  const sourceFile = file(state);
  const selected = sourceFile.annotations.filter(annotation =>
    sourceFile.selection.ids.includes(annotation.id)
  );
  const summary = deleteSummary(state);
  if (!selected.length) return summary;
  return command(state, '批量软删除对象', () => {
    selected.forEach(annotation => {
      annotation.deleted = true;
      recordRevision(annotation, 'SoftDelete', {}, true);
    });
    clearSelection(state);
    return summary;
  });
}

export function undo(state) {
  const entry = state.undo.at(-1);
  if (!entry) return false;
  applyCommandDifferences(state, entry.changes, 'before');
  state.undo.pop();
  state.redo.push(entry);
  return true;
}

export function redo(state) {
  const entry = state.redo.at(-1);
  if (!entry) return false;
  applyCommandDifferences(state, entry.changes, 'after');
  state.redo.pop();
  state.undo.push(entry);
  return true;
}

export function defaultChannelRequest(state) {
  const sourceFile = file(state);
  if (sourceFile.selection.ids.length !== 1 || !active(state)) {
    fail('创建通道需要且只能选择一个未删除对象。');
  }
  const annotation = active(state);
  const currentSegment = segment(state, annotation);
  let referenceRange;
  let range;
  if (currentSegment) {
    referenceRange = copy(currentSegment.range);
    range = copy(currentSegment.range);
  } else {
    if (!annotation.observation)
      fail('该对象没有活动段或观察带，无法建立通道请求。');
    referenceRange = copy(annotation.observation);
    const startSample = Math.min(
      sourceFile.availableSamples,
      Math.max(
        0,
        Math.round((sourceFile.view.startMs * sourceFile.sampleRateHz) / 1000)
      )
    );
    const endSample = Math.max(
      startSample,
      Math.min(
        sourceFile.availableSamples,
        Math.round((sourceFile.view.endMs * sourceFile.sampleRateHz) / 1000)
      )
    );
    range = { ...annotation.observation, startSample, endSample };
  }
  const bandwidthHz = range.frequencyUpperHz - range.frequencyLowerHz;
  return {
    name: `${annotation.name} · 通道`,
    annotationId: annotation.id,
    segmentId: currentSegment?.id ?? null,
    scopeOrigin: currentSegment ? 'LocatedSegment' : 'UserRange',
    sourceRevisionId: annotation.revisionId,
    range,
    referenceRange,
    centerFrequencyHz: (range.frequencyLowerHz + range.frequencyUpperHz) / 2,
    bandwidthHz,
    outputSampleRateHz: Math.min(
      sourceFile.sampleRateHz,
      Math.max(4e6, 1.3 * bandwidthHz)
    ),
    filter: 'standard',
  };
}

export function validateChannelRequest(state, request) {
  const sourceFile = file(state);
  const errors = [];
  const add = (code, field, message) => errors.push({ code, field, message });
  if (!sourceFile)
    return [
      { code: 'FileMissing', field: 'source', message: '当前文件不存在。' },
    ];
  if (!isRecord(request))
    return [
      {
        code: 'RequestInvalid',
        field: 'request',
        message: '通道请求必须为对象。',
      },
    ];
  const annotation = sourceFile.annotations.find(
    value => value.id === request.annotationId && !value.deleted
  );
  if (!annotation)
    add('AnnotationMissing', 'annotationId', '绑定对象不存在或已经删除。');
  if (!sourceFile.sourceAvailable)
    add('SourceMissing', 'source', '演示物理源不可用，不能创建提取通道。');
  if (annotation && annotation.sourceIdentity !== sourceFile.sourceIdentity) {
    add('SourceChanged', 'source', '物理源身份已变化，当前标记属于原物理源。');
  }
  if (typeof request.name !== 'string' || !request.name.trim())
    add('NameEmpty', 'name', '通道名称不能为空。');
  if (
    typeof request.sourceRevisionId !== 'string' ||
    !request.sourceRevisionId
  ) {
    add('RevisionMissing', 'sourceRevisionId', '通道请求缺少来源修订标识。');
  }
  let binding = null;
  if (request.scopeOrigin === 'LocatedSegment') {
    binding =
      annotation?.segments.find(part => part.id === request.segmentId)?.range ??
      null;
    if (!binding)
      add('SegmentMissing', 'segmentId', '绑定的明确活动段不存在。');
  } else if (request.scopeOrigin === 'UserRange') {
    if (request.segmentId !== null && request.segmentId !== undefined) {
      add('UnexpectedSegment', 'segmentId', '用户手工范围不能伪造活动段绑定。');
    }
    if (annotation?.segments.length) {
      add(
        'ScopeChanged',
        'scopeOrigin',
        '对象已有明确活动段，请重新选择段并打开通道请求。'
      );
    }
    binding = annotation?.observation ?? null;
    if (annotation && !binding)
      add('ObservationMissing', 'scopeOrigin', '对象缺少时间未定位观察带。');
  } else
    add(
      'ScopeInvalid',
      'scopeOrigin',
      '范围来源必须为明确活动段或用户手工范围。'
    );
  if (binding && !sameRange(binding, request.referenceRange)) {
    add(
      'BindingChanged',
      'referenceRange',
      '绑定几何已变化，请重新打开通道请求。'
    );
  }
  const range = request.range;
  let timeValid = true;
  let frequencyValid = true;
  if (!isRecord(range)) {
    add('RangeMissing', 'range', '通道提取范围缺失。');
    timeValid = false;
    frequencyValid = false;
  } else {
    if (!integer(range.startSample) || !integer(range.endSample)) {
      add('SampleInteger', 'range', '提取样本起止必须为安全整数。');
      timeValid = false;
    } else {
      if (range.startSample >= range.endSample) {
        add(
          'EmptyRange',
          'range',
          '当前视图与可读前缀没有非空交集，请输入非空提取时间范围。'
        );
        timeValid = false;
      }
      if (range.startSample < 0 || range.endSample > sourceFile.totalSamples) {
        add('PhysicalBounds', 'range', '提取范围超出文件物理样本边界。');
        timeValid = false;
      }
      if (
        range.startSample < 0 ||
        range.endSample > sourceFile.availableSamples ||
        sourceFile.availableSamples <= 0
      ) {
        add('UnreadableRange', 'range', '提取范围超出当前可读样本前缀。');
        timeValid = false;
      }
    }
    if (
      !finite(range.frequencyLowerHz) ||
      !finite(range.frequencyUpperHz) ||
      range.frequencyLowerHz >= range.frequencyUpperHz
    ) {
      add('FrequencyRange', 'range', '提取频率下界必须小于有限的频率上界。');
      frequencyValid = false;
    }
  }
  if (
    binding &&
    request.scopeOrigin === 'LocatedSegment' &&
    timeValid &&
    frequencyValid &&
    !rangeInside(range, binding)
  ) {
    add('OutsideSegment', 'range', '明确活动段的提取范围只能在该段内裁剪。');
  }
  if (!finite(request.centerFrequencyHz)) {
    add('CenterInvalid', 'centerFrequencyHz', '通道中心频率必须为有限数值。');
  }
  if (!finite(request.bandwidthHz) || request.bandwidthHz <= 0) {
    add('BandwidthInvalid', 'bandwidthHz', '通道带宽必须大于零。');
  }
  if (
    frequencyValid &&
    finite(request.centerFrequencyHz) &&
    finite(request.bandwidthHz) &&
    request.bandwidthHz > 0
  ) {
    const expectedCenter =
      (range.frequencyLowerHz + range.frequencyUpperHz) / 2;
    const expectedBandwidth = range.frequencyUpperHz - range.frequencyLowerHz;
    if (
      Math.abs(expectedCenter - request.centerFrequencyHz) > 0.001 ||
      Math.abs(expectedBandwidth - request.bandwidthHz) > 0.001
    ) {
      add(
        'FrequencyMismatch',
        'range',
        '通道中心频率、带宽和提取频率范围必须一致。'
      );
    }
    const guard = request.bandwidthHz * 0.15;
    const lower = sourceFile.centerFrequencyHz - sourceFile.sampleRateHz / 2;
    const upper = sourceFile.centerFrequencyHz + sourceFile.sampleRateHz / 2;
    if (
      request.centerFrequencyHz - request.bandwidthHz / 2 - guard < lower ||
      request.centerFrequencyHz + request.bandwidthHz / 2 + guard > upper
    ) {
      add(
        'FilterGuard',
        'bandwidthHz',
        '通道频带及每侧 15% 滤波保护带必须位于输入采样频带内。'
      );
    }
  }
  if (!finite(request.outputSampleRateHz) || request.outputSampleRateHz <= 0) {
    add('OutputRateInvalid', 'outputSampleRateHz', '输出采样率必须大于零。');
  } else {
    if (
      finite(request.bandwidthHz) &&
      request.outputSampleRateHz < 1.3 * request.bandwidthHz
    ) {
      add(
        'OutputRateLow',
        'outputSampleRateHz',
        '输出采样率至少需要通道带宽的 1.3 倍。'
      );
    }
    if (request.outputSampleRateHz > sourceFile.sampleRateHz) {
      add(
        'OutputRateHigh',
        'outputSampleRateHz',
        '输出采样率不能高于输入采样率。'
      );
    }
  }
  if (request.filter !== 'standard')
    add('FilterInvalid', 'filter', '本演示仅支持 standard 滤波参数。');
  return errors;
}

function channelFromRequest(sourceFile, annotation, id, request) {
  return {
    id,
    name: request.name.trim(),
    annotationId: annotation.id,
    segmentId: request.segmentId ?? null,
    scopeOrigin: request.scopeOrigin,
    sourceSnapshot: freeze({
      sourceIdentity: sourceFile.sourceIdentity,
      revisionId: request.sourceRevisionId,
      referenceRange: copy(request.referenceRange),
      range: copy(request.range),
    }),
    range: copy(request.range),
    centerFrequencyHz: request.centerFrequencyHz,
    bandwidthHz: request.bandwidthHz,
    outputSampleRateHz: request.outputSampleRateHz,
    filter: request.filter,
    demoOnly: true,
    createdAt: 'DEMO',
  };
}

export function createChannel(state, request) {
  const errors = validateChannelRequest(state, request);
  if (errors.length) fail(errors.map(error => error.message).join(' '));
  const sourceFile = file(state);
  const annotation = annotationById(state, request.annotationId);
  let ordinal = 1;
  while (
    sourceFile.channels.some(channel => channel.id === `channel-${ordinal}`)
  )
    ordinal += 1;
  const created = channelFromRequest(
    sourceFile,
    annotation,
    `channel-${ordinal}`,
    request
  );
  return command(state, '创建演示通道', () => {
    sourceFile.channels.push(created);
    return created;
  });
}

export function channelRelation(sourceFile, channel) {
  const annotation = sourceFile.annotations.find(
    value => value.id === channel.annotationId
  );
  if (!annotation || annotation.deleted) return 'Missing';
  const boundSegment =
    channel.scopeOrigin === 'LocatedSegment'
      ? annotation.segments.find(part => part.id === channel.segmentId)
      : null;
  if (channel.scopeOrigin === 'LocatedSegment' && !boundSegment)
    return 'Missing';
  if (
    !sourceFile.sourceAvailable ||
    sourceFile.sourceIdentity !== channel.sourceSnapshot.sourceIdentity ||
    annotation.sourceIdentity !== sourceFile.sourceIdentity
  )
    return 'Unavailable';
  if (channel.scopeOrigin === 'UserRange' && annotation.segments.length)
    return 'Changed';
  const boundRange = boundSegment?.range ?? annotation.observation;
  return sameRange(boundRange, channel.sourceSnapshot.referenceRange)
    ? 'Current'
    : 'Changed';
}

export function snapshot(state) {
  const result = copy(state);
  delete result.undo;
  delete result.redo;
  for (const sourceFile of result.files) {
    delete sourceFile.selection.hoverId;
    delete sourceFile.selection.hoverSegmentId;
  }
  return result;
}

export function save(state) {
  return snapshot(state);
}

function checkString(value, label, { empty = false } = {}) {
  if (typeof value !== 'string' || (!empty && !value.trim()))
    fail(`${label}必须为${empty ? '' : '非空'}文本。`);
}

function checkSegment(sourceFile, part, label) {
  if (!isRecord(part)) fail(`${label}必须为活动段对象。`);
  checkString(part.id, `${label}标识`);
  checkedRange(sourceFile, part.range, `${label}范围`);
}

function uniqueIds(items, label) {
  const ids = new Set();
  for (const item of items) {
    if (!isRecord(item)) fail(`${label}必须为对象数组。`);
    checkString(item.id, `${label}标识`);
    if (ids.has(item.id)) fail(`${label}存在重复标识 ${item.id}。`);
    ids.add(item.id);
  }
}

function validateSavedFile(sourceFile) {
  if (!isRecord(sourceFile)) fail('文件记录必须为对象。');
  checkString(sourceFile.id, '文件标识');
  checkString(sourceFile.name, '文件名称');
  if (
    !finite(sourceFile.sampleRateHz) ||
    sourceFile.sampleRateHz <= 0 ||
    !finite(sourceFile.centerFrequencyHz) ||
    !integer(sourceFile.totalSamples) ||
    sourceFile.totalSamples <= 0 ||
    !integer(sourceFile.availableSamples) ||
    sourceFile.availableSamples < 0 ||
    sourceFile.availableSamples > sourceFile.totalSamples
  ) {
    fail(`文件 ${sourceFile.id} 的采样参数、物理长度或可读前缀无效。`);
  }
  if (typeof sourceFile.sourceAvailable !== 'boolean')
    fail('文件可用状态必须为布尔值。');
  checkString(sourceFile.sourceIdentity, '文件物理源身份');
  fromDisplayRange(sourceFile, sourceFile.view);
  if (
    !Array.isArray(sourceFile.annotations) ||
    !Array.isArray(sourceFile.channels)
  ) {
    fail('文件标记及通道必须为数组。');
  }
  uniqueIds(sourceFile.annotations, '文件标记');
  uniqueIds(sourceFile.channels, '文件通道');
  for (const annotation of sourceFile.annotations) {
    checkString(annotation.name, '标记名称');
    checkString(annotation.note, '标记备注', { empty: true });
    checkString(annotation.sourceIdentity, '标记物理源身份');
    checkString(annotation.revisionId, '标记修订标识');
    if (
      !SOURCES.includes(annotation.sourceKind) ||
      !QUALITIES.includes(annotation.reviewStatus) ||
      !integer(annotation.revisionNumber) ||
      annotation.revisionNumber < 0 ||
      typeof annotation.deleted !== 'boolean' ||
      !Array.isArray(annotation.segments) ||
      !Array.isArray(annotation.revisionHistory) ||
      !Array.isArray(annotation.qualityHistory) ||
      (annotation.score !== null &&
        (!finite(annotation.score) ||
          annotation.score < 0 ||
          annotation.score > 1))
    )
      fail(`标记 ${annotation.id} 的属性无效。`);
    uniqueIds(annotation.segments, '当前活动段');
    annotation.segments.forEach(part =>
      checkSegment(sourceFile, part, '当前活动段')
    );
    if (annotation.observation !== null)
      checkedRange(sourceFile, annotation.observation, '观察带范围');
    if (!annotation.segments.length && !annotation.observation) {
      fail(`标记 ${annotation.id} 必须具有活动段或时间未定位观察带。`);
    }
    if (
      (annotation.activeSegmentId !== null &&
        !annotation.segments.some(
          part => part.id === annotation.activeSegmentId
        )) ||
      (!annotation.segments.length && annotation.activeSegmentId !== null)
    ) {
      fail(`标记 ${annotation.id} 的当前活动段标识无效。`);
    }
    if (
      !isRecord(annotation.origin) ||
      !Array.isArray(annotation.origin.segments) ||
      !isRecord(annotation.origin.evidence)
    )
      fail(`标记 ${annotation.id} 缺少原始证据。`);
    checkString(annotation.origin.name, '原始标记名称');
    uniqueIds(annotation.origin.segments, '原始活动段');
    annotation.origin.segments.forEach(part =>
      checkSegment(sourceFile, part, '原始活动段')
    );
    if (annotation.origin.observation !== null) {
      checkedRange(sourceFile, annotation.origin.observation, '原始观察带范围');
    }
    if (!annotation.origin.segments.length && !annotation.origin.observation) {
      fail(`标记 ${annotation.id} 缺少原始几何证据。`);
    }
    for (const revision of annotation.revisionHistory) {
      if (!isRecord(revision) || !Array.isArray(revision.segments))
        fail('标记修订历史格式无效。');
      checkString(revision.revisionId, '历史修订标识');
      checkString(revision.previousRevisionId, '历史前序修订标识');
      if (!integer(revision.revisionNumber) || revision.revisionNumber < 1)
        fail('历史修订号无效。');
      uniqueIds(revision.segments, '历史活动段');
      revision.segments.forEach(part =>
        checkSegment(sourceFile, part, '历史活动段')
      );
      if (revision.observation !== null)
        checkedRange(sourceFile, revision.observation, '历史观察带范围');
    }
    for (const quality of annotation.qualityHistory) {
      if (
        !isRecord(quality) ||
        !QUALITIES.includes(quality.from) ||
        !QUALITIES.includes(quality.to)
      ) {
        fail('标记复核历史格式无效。');
      }
      checkString(quality.revisionId, '复核历史修订标识');
    }
  }
  for (const channel of sourceFile.channels) {
    checkString(channel.name, '通道名称');
    if (
      !sourceFile.annotations.some(
        annotation => annotation.id === channel.annotationId
      )
    ) {
      fail(`通道 ${channel.id} 绑定的标记不存在。`);
    }
    if (
      !['LocatedSegment', 'UserRange'].includes(channel.scopeOrigin) ||
      (channel.scopeOrigin === 'LocatedSegment' &&
        (typeof channel.segmentId !== 'string' || !channel.segmentId)) ||
      (channel.scopeOrigin === 'UserRange' && channel.segmentId !== null) ||
      !isRecord(channel.sourceSnapshot) ||
      channel.demoOnly !== true
    )
      fail('演示通道绑定格式无效。');
    checkString(channel.sourceSnapshot.sourceIdentity, '通道快照物理源身份');
    checkString(channel.sourceSnapshot.revisionId, '通道快照修订标识');
    checkedRange(
      sourceFile,
      channel.sourceSnapshot.referenceRange,
      '通道原绑定范围'
    );
    checkedRange(sourceFile, channel.sourceSnapshot.range, '通道手工提取范围');
    const actual = checkedRange(sourceFile, channel.range, '通道实际提取范围');
    if (!sameRange(actual, channel.sourceSnapshot.range))
      fail('通道实际范围与创建快照不一致。');
    if (
      !finite(channel.centerFrequencyHz) ||
      !finite(channel.bandwidthHz) ||
      channel.bandwidthHz <= 0 ||
      !finite(channel.outputSampleRateHz) ||
      channel.outputSampleRateHz < 1.3 * channel.bandwidthHz ||
      channel.outputSampleRateHz > sourceFile.sampleRateHz ||
      channel.filter !== 'standard'
    ) {
      fail('通道中心频率、带宽、输出采样率或滤波参数无效。');
    }
    const center = (actual.frequencyLowerHz + actual.frequencyUpperHz) / 2;
    const bandwidth = actual.frequencyUpperHz - actual.frequencyLowerHz;
    if (
      Math.abs(channel.centerFrequencyHz - center) > 0.001 ||
      Math.abs(channel.bandwidthHz - bandwidth) > 0.001
    )
      fail('通道频率参数与实际提取范围不一致。');
    const guard = bandwidth * 0.15;
    if (
      actual.frequencyLowerHz - guard <
        sourceFile.centerFrequencyHz - sourceFile.sampleRateHz / 2 ||
      actual.frequencyUpperHz + guard >
        sourceFile.centerFrequencyHz + sourceFile.sampleRateHz / 2
    ) {
      fail('通道频带及滤波保护带超出输入采样频带。');
    }
  }
  const selected = sourceFile.selection;
  if (
    !isRecord(selected) ||
    !Array.isArray(selected.ids) ||
    selected.ids.some(id => typeof id !== 'string') ||
    !['activeId', 'segmentId', 'anchorId'].every(
      key => selected[key] === null || typeof selected[key] === 'string'
    )
  )
    fail('文件选择状态格式无效。');
}

export function load(textOrObject) {
  let candidate;
  try {
    candidate =
      typeof textOrObject === 'string'
        ? JSON.parse(textOrObject)
        : copy(textOrObject);
  } catch {
    fail('无法读取演示状态：JSON 文本或对象无效。');
  }
  if (
    !isRecord(candidate) ||
    candidate.schema !== SCHEMA ||
    candidate.version !== 6 ||
    candidate.demoOnly !== true
  )
    fail('仅支持本交互演示的 schema、version 6 和 demoOnly 状态。');
  if (!SCENARIOS.includes(candidate.scenario)) fail('演示状态的场景标识无效。');
  if (!Array.isArray(candidate.files) || !candidate.files.length)
    fail('演示状态至少需要一个文件。');
  uniqueIds(candidate.files, '文件');
  candidate.files.forEach(validateSavedFile);
  if (
    !candidate.files.some(
      sourceFile => sourceFile.id === candidate.activeFileId
    )
  )
    fail('当前文件标识不存在。');
  checkedFilters(candidate.filters);
  if (
    !isRecord(candidate.sort) ||
    !SORT_KEYS.includes(candidate.sort.key) ||
    typeof candidate.sort.ascending !== 'boolean'
  )
    fail('演示状态的排序设置无效。');
  if (
    !integer(candidate.page) ||
    candidate.page < 0 ||
    candidate.pageSize !== 200 ||
    !isRecord(candidate.treeLimit) ||
    !SOURCES.every(
      key => integer(candidate.treeLimit[key]) && candidate.treeLimit[key] >= 1
    ) ||
    !['spectrogram', 'waterfall', 'spectrum', 'waveform'].includes(
      candidate.mode
    )
  ) {
    fail('演示状态的分页、树展开数量或显示模式无效。');
  }
  candidate.undo = [];
  candidate.redo = [];
  const currentFileId = candidate.activeFileId;
  for (const sourceFile of candidate.files) {
    sourceFile.selection.hoverId = null;
    sourceFile.selection.hoverSegmentId = null;
    candidate.activeFileId = sourceFile.id;
    reconcileSelection(candidate);
  }
  candidate.activeFileId = currentFileId;
  freezeOrigins(candidate);
  reconcileSelection(candidate);
  ensurePageForActive(candidate);
  return candidate;
}
