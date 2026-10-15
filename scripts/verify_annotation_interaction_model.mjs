import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import * as model from '../docs/prototype/annotation-interactions-u0/model.mjs';

const results = [];
const startedAt = new Date().toISOString();
const clone = value => JSON.parse(JSON.stringify(value));
const find = (state, id) =>
  model.file(state).annotations.find(annotation => annotation.id === id);
const range = (state, startMs, endMs, lowerMHz = 2502, upperMHz = 2502.34) =>
  model.fromDisplayRange(model.file(state), {
    startMs,
    endMs,
    lowerMHz,
    upperMHz,
  });
const relation = (state, index = 0) =>
  model.channelRelation(model.file(state), model.file(state).channels[index]);
const codes = (state, request) =>
  model.validateChannelRequest(state, request).map(error => error.code);

function publishSyntheticAnnotation(sourceFile, id) {
  const published = clone(
    model
      .createState()
      .files[0].annotations.find(annotation => annotation.id === 'algo-located')
  );
  published.id = id;
  published.name = `后台发布演示 · ${id}`;
  published.revisionId = `${id}-r0`;
  published.sourceIdentity = sourceFile.sourceIdentity;
  published.segments.forEach((part, index) => {
    part.id = `${id}-s${index + 1}`;
  });
  published.activeSegmentId = published.segments[0].id;
  published.origin.name = published.name;
  published.origin.segments = clone(published.segments);
  sourceFile.annotations.push(published);
  return published;
}

function publishSyntheticChannel(sourceFile, annotation, id) {
  const published = clone(model.createState().files[0].channels[0]);
  published.id = id;
  published.name = `后台发布通道 · ${id}`;
  published.annotationId = annotation.id;
  published.segmentId = annotation.segments[0].id;
  published.range = clone(annotation.segments[0].range);
  published.sourceSnapshot = {
    sourceIdentity: sourceFile.sourceIdentity,
    revisionId: annotation.revisionId,
    referenceRange: clone(published.range),
    range: clone(published.range),
  };
  sourceFile.channels.push(published);
  return published;
}

function test(name, verify) {
  const started = performance.now();
  try {
    verify();
    results.push({
      name,
      passed: true,
      durationMs: Number((performance.now() - started).toFixed(3)),
    });
  } catch (error) {
    results.push({
      name,
      passed: false,
      durationMs: Number((performance.now() - started).toFixed(3)),
      message: error.message,
      stack: error.stack?.split('\n').slice(0, 7).join('\n'),
    });
  }
}

function assertSelection(state) {
  const sourceFile = model.file(state);
  const selected = sourceFile.selection;
  const visible = new Set(
    model.visibleAnnotations(state).map(annotation => annotation.id)
  );
  assert.equal(new Set(selected.ids).size, selected.ids.length);
  selected.ids.forEach(id =>
    assert(visible.has(id), `隐藏或删除对象不得保留选择：${id}`)
  );
  assert.equal(
    selected.ids.includes(selected.activeId),
    selected.activeId !== null
  );
  const current = model.active(state);
  assert.equal(selected.segmentId, current?.activeSegmentId ?? null);
  if (current)
    assert.equal(model.segment(state)?.id ?? null, selected.segmentId);
}

function updateFrequency(request, lowerHz, upperHz) {
  request.range.frequencyLowerHz = lowerHz;
  request.range.frequencyUpperHz = upperHz;
  request.centerFrequencyHz = (lowerHz + upperHz) / 2;
  request.bandwidthHz = upperHz - lowerHz;
}

test('standard 夹具独立包含四条标签与三来源八个对象', () => {
  const state = model.createState();
  const sourceFile = model.file(state);
  assert.equal(state.schema, 'signal-studio-annotation-interaction-prototype');
  assert.equal(state.version, 6);
  assert.equal(state.demoOnly, true);
  assert.equal(sourceFile.sampleRateHz, 20e6);
  assert.equal(sourceFile.totalSamples, 400000);
  assert.equal(sourceFile.availableSamples, 400000);
  assert.equal(sourceFile.annotations.length, 8);
  assert.deepEqual(
    sourceFile.annotations
      .filter(annotation => annotation.sourceKind === 'ImportedLabel')
      .map(annotation => [
        annotation.segments[0].range.startSample,
        annotation.segments[0].range.endSample,
      ]),
    [
      [51107, 343277],
      [38498, 353824],
      [70248, 359981],
      [173404, 345294],
    ]
  );
  assert.equal(model.active(state).id, 'algo-observation');
  assert.equal(model.segment(state), null);
  assert.equal(relation(state), 'Current');
  assert.equal(sourceFile.channels[0].segmentId, 'algo-located-s1');
  assertSelection(state);
});

test('各次 createState 夹具、origin 与选择互不共享', () => {
  const left = model.createState();
  const right = model.createState();
  model.edit(left, 'algo-observation', { name: '仅修改左侧' });
  assert.notEqual(model.active(left).name, model.active(right).name);
  assert(Object.isFrozen(model.active(left).origin));
  assert(Object.isFrozen(find(left, 'algo-located').origin.segments[0].range));
  assert.throws(() => {
    model.active(left).origin.name = '禁止篡改原始证据';
  }, TypeError);
  assert.throws(() => model.createState('真实模型'), /不支持的演示场景/);
});

test('毫秒/MHz 显示换算保留物理整数样本和小数 Hz', () => {
  const state = model.createState();
  const original = find(state, 'label-1').segments[0].range;
  const display = model.toDisplayRange(model.file(state), original);
  const converted = model.fromDisplayRange(model.file(state), display);
  assert.equal(converted.startSample, original.startSample);
  assert.equal(converted.endSample, original.endSample);
  assert(
    Math.abs(converted.frequencyLowerHz - original.frequencyLowerHz) < 1e-5
  );
  assert(
    Math.abs(converted.frequencyUpperHz - original.frequencyUpperHz) < 1e-5
  );
  assert.equal(range(state, 0.00003, 1).startSample, 1);
  assert.throws(() => range(state, 4, 4), /起点小于终点/);
  assert.throws(() => range(state, 0, 1, 2480, 2490), /采样频带/);
  assert.throws(
    () => model.fromDisplayRange(model.file(state), { startMs: '4' }),
    /有限/
  );
});

test('单选与多段活动项保持对象/文件段 ID 同步', () => {
  const state = model.createState();
  model.select(state, 'manual-1', { segmentId: 'manual-1-s2' });
  assert.equal(model.segment(state).id, 'manual-1-s2');
  assertSelection(state);
  model.select(state, 'algo-located');
  model.select(state, 'manual-1');
  assert.equal(model.segment(state).id, 'manual-1-s2');
  assertSelection(state);
  const before = model.save(state);
  assert.throws(
    () => model.select(state, 'manual-1', { segmentId: 'algo-located-s1' }),
    /不包含/
  );
  assert.deepEqual(model.save(state), before);
});

test('Ctrl 点击切换选择并维护有效 active/anchor', () => {
  const state = model.createState();
  model.select(state, 'manual-1');
  model.select(state, 'algo-located', {
    ctrlKey: true,
    segmentId: 'algo-located-s2',
  });
  assert.equal(model.file(state).selection.ids.length, 2);
  assert.equal(model.active(state).id, 'algo-located');
  assert.equal(model.segment(state).id, 'algo-located-s2');
  model.select(state, 'algo-located', { ctrlKey: true });
  assert.equal(model.active(state).id, 'manual-1');
  assertSelection(state);
  model.select(state, 'manual-1', { ctrlKey: true });
  assert.equal(model.active(state), null);
  assertSelection(state);
});

test('Shift 采用指定可见顺序连续选择，Ctrl+Shift 取并集', () => {
  const state = model.createState();
  const order = [
    'manual-1',
    'label-1',
    'algo-observation',
    'algo-located',
    'label-2',
  ];
  model.select(state, 'manual-1');
  model.select(state, 'algo-located', { shiftKey: true, orderedIds: order });
  assert.deepEqual(
    new Set(model.file(state).selection.ids),
    new Set(order.slice(0, 4))
  );
  assert.equal(model.file(state).selection.anchorId, 'manual-1');
  model.select(state, 'label-2', { ctrlKey: true });
  model.select(state, 'algo-located', {
    ctrlKey: true,
    shiftKey: true,
    orderedIds: order,
  });
  assert(model.file(state).selection.ids.includes('manual-1'));
  assert(model.file(state).selection.ids.includes('label-2'));
  assertSelection(state);
});

test('large 跨 200 行分页定位对象，全选覆盖全部匹配项', () => {
  const state = model.createState('large');
  assert.equal(model.visibleAnnotations(state).length, 268);
  assert.equal(
    model
      .file(state)
      .annotations.filter(annotation => /^bulk-\d{3}$/.test(annotation.id))
      .length,
    260
  );
  assert.equal(
    model
      .file(state)
      .annotations.filter(annotation => annotation.sourceKind === 'Algorithm')
      .length,
    263
  );
  assert(
    model
      .file(state)
      .annotations.filter(annotation => /^bulk-\d{3}$/.test(annotation.id))
      .every(annotation => annotation.sourceKind === 'Algorithm')
  );
  model.select(state, 'bulk-260');
  assert.equal(state.page, 1);
  model.selectAll(state);
  assert.equal(model.file(state).selection.ids.length, 268);
  assertSelection(state);
  model.clearSelection(state);
  assertSelection(state);
});

test('评分大于 0 时排除无评分对象；来源/质量/图层组合正确', () => {
  const state = model.createState();
  model.applyFilter(state, { score: 0.8 });
  assert.deepEqual(
    new Set(model.visibleAnnotations(state).map(annotation => annotation.id)),
    new Set(['algo-located', 'algo-observation'])
  );
  model.applyFilter(state, { score: 0, source: 'Manual' });
  assert.deepEqual(
    model.visibleAnnotations(state).map(annotation => annotation.id),
    ['manual-1']
  );
  model.applyFilter(state, { layers: { Manual: false } });
  assert.equal(model.visibleAnnotations(state).length, 0);
  model.applyFilter(state, { source: 'all', quality: 'Rejected' });
  assert.deepEqual(
    model.visibleAnnotations(state).map(annotation => annotation.id),
    ['algo-ignored']
  );
  assert.throws(() => model.applyFilter(state, { score: 1.2 }), /筛选条件无效/);
  assert.throws(
    () => model.applyFilter(state, { status: 'Pending' }),
    /不支持的字段/
  );
});

test('非法图层补丁具体报错且不覆盖已提交筛选', () => {
  const state = model.createState();
  const before = clone(state.filters);
  for (const layers of [false, 'Manual', { Manual: 1 }, { Unknown: true }]) {
    assert.throws(() => model.applyFilter(state, { layers }), /图层补丁/);
    assert.deepEqual(state.filters, before);
  }
});

test('筛选、隐藏图层和外部删除后协调选择，隐藏对象不可直接选择', () => {
  const state = model.createState();
  model.selectAll(state);
  model.applyFilter(state, { source: 'Manual' });
  assert.deepEqual(model.file(state).selection.ids, ['manual-1']);
  assert.throws(() => model.select(state, 'label-1'), /可见结果/);
  find(state, 'manual-1').deleted = true;
  model.reconcileSelection(state);
  assertSelection(state);
  assert.equal(model.active(state), null);
});

test('排序只改变展示顺序，保持选择并按 active 重新定位分页', () => {
  const state = model.createState('large');
  model.select(state, 'bulk-260');
  model.sortBy(state, 'name');
  assert.equal(state.sort.ascending, false);
  assert.equal(state.page, 0);
  assert.equal(model.active(state).id, 'bulk-260');
  model.sortBy(state, 'startMs');
  assert.equal(state.sort.ascending, true);
  const sorted = model.visibleAnnotations(state);
  assert.equal(sorted.at(-1).id, 'algo-observation');
  const values = sorted
    .filter(annotation => annotation.segments.length)
    .map(
      annotation =>
        annotation.segments.find(part => part.id === annotation.activeSegmentId)
          .range.startSample
    );
  assert(
    values.every((value, index) => index === 0 || value >= values[index - 1])
  );
  assertSelection(state);
  assert.throws(() => model.sortBy(state, '不存在字段'), /不支持/);
});

test('多段切换后起止排序立即使用当前段，不使用全对象包围范围', () => {
  const state = model.createState();
  const index = id =>
    model
      .visibleAnnotations(state)
      .findIndex(annotation => annotation.id === id);
  model.select(state, 'algo-located', { segmentId: 'algo-located-s1' });
  model.sortBy(state, 'startMs');
  assert(index('algo-located') < index('label-3'));
  model.select(state, 'algo-located', { segmentId: 'algo-located-s2' });
  assert(index('algo-located') > index('label-3'));
  model.sortBy(state, 'startSample');
  assert(index('algo-located') > index('label-3'));
  model.sortBy(state, 'endMs');
  assert(index('algo-located') > index('algo-ignored'));
  model.select(state, 'algo-located', { segmentId: 'algo-located-s1' });
  assert(index('algo-located') < index('algo-ignored'));
  model.sortBy(state, 'endSample');
  assert(index('algo-located') < index('algo-ignored'));
  assertSelection(state);
});

test('未知时间的起止值在升序降序均置尾，不把观察支持当活动时间', () => {
  const state = model.createState();
  for (const key of ['startMs', 'endMs', 'startSample', 'endSample']) {
    model.sortBy(state, key);
    for (const ascending of [true, false]) {
      if (state.sort.ascending !== ascending) model.sortBy(state, key);
      const sorted = model.visibleAnnotations(state);
      assert.equal(sorted.at(-1).id, 'algo-observation');
      const values = sorted.slice(0, -1).map(annotation => {
        const current = annotation.segments.find(
          part => part.id === annotation.activeSegmentId
        ).range;
        return key.startsWith('start')
          ? current.startSample
          : current.endSample;
      });
      assert(
        values.every(
          (value, index) =>
            index === 0 ||
            (ascending
              ? value >= values[index - 1]
              : value <= values[index - 1])
        )
      );
      assertSelection(state);
    }
  }
});

test('中心频率和带宽按当前段排序，未知时间对象仍使用观察带频率', () => {
  const state = model.createState();
  const index = id =>
    model
      .visibleAnnotations(state)
      .findIndex(annotation => annotation.id === id);
  model.select(state, 'algo-located', { segmentId: 'algo-located-s2' });
  model.edit(state, 'algo-located', {
    segmentId: 'algo-located-s2',
    range: range(state, 9.4, 16.6, 2495, 2496),
  });
  model.sortBy(state, 'centerMHz');
  assert(index('algo-located') < index('manual-1'));
  assert(index('manual-1') < index('algo-observation'));
  model.select(state, 'algo-located', { segmentId: 'algo-located-s1' });
  assert(index('algo-located') > index('algo-observation'));
  model.sortBy(state, 'frequencyLowerHz');
  assert(index('algo-located') > index('algo-observation'));
  model.sortBy(state, 'bandwidthKHz');
  assert(index('algo-located') < index('algo-observation'));
  assert(index('algo-observation') < index('manual-1'));
  model.select(state, 'algo-located', { segmentId: 'algo-located-s2' });
  assert(index('algo-located') > index('manual-1'));
  assertSelection(state);
});

test('文件之间选择、段 ID 与 20/60 Msps 换算隔离', () => {
  const state = model.createState();
  model.select(state, 'manual-1', { segmentId: 'manual-1-s2' });
  model.switchFile(state, 'file-b');
  assert.equal(model.file(state).sampleRateHz, 60e6);
  assert.equal(model.active(state), null);
  assert.equal(range(state, 1, 2).endSample, 120000);
  const created = model.createManual(state, range(state, 1, 2));
  assert.equal(created.sourceIdentity, 'demo-file-b-v1');
  model.switchFile(state, 'file-a');
  assert.equal(model.active(state).id, 'manual-1');
  assert.equal(model.segment(state).id, 'manual-1-s2');
  assertSelection(state);
  assert.throws(() => model.switchFile(state, 'file-c'), /不存在/);
});

test('名称和备注自动生成修订但保持质量、来源与通道 Current', () => {
  const state = model.createState();
  model.select(state, 'algo-located');
  model.setQuality(state, 'Accepted');
  const original = clone(model.active(state).origin);
  const revisionId = model.active(state).revisionId;
  model.edit(
    state,
    'algo-located',
    { name: ' 新名称 ', note: '独立备注' },
    revisionId
  );
  assert.equal(model.active(state).name, '新名称');
  assert.equal(model.active(state).reviewStatus, 'Accepted');
  assert.equal(model.active(state).sourceKind, 'Algorithm');
  assert.equal(model.active(state).id, 'algo-located');
  assert.notEqual(model.active(state).revisionId, revisionId);
  assert.equal(model.active(state).revisionNumber, 1);
  assert.deepEqual(model.active(state).origin, original);
  assert.equal(relation(state), 'Current');
  assert.throws(
    () => model.edit(state, 'algo-located', { note: '过期' }, revisionId),
    /过期/
  );
  assert.equal(model.active(state).note, '独立备注');
});

test('空编辑不写撤销项，非法编辑保持数据和 undo 原子性', () => {
  const state = model.createState();
  model.select(state, 'algo-located');
  const original = model.save(state);
  model.edit(state, 'algo-located', {
    name: model.active(state).name,
    range: clone(model.segment(state).range),
  });
  assert.equal(state.undo.length, 0);
  assert.deepEqual(model.save(state), original);
  assert.throws(
    () => model.edit(state, 'algo-located', { name: '   ' }),
    /名称不能为空/
  );
  assert.throws(
    () => model.edit(state, 'algo-located', { sourceKind: 'Manual' }),
    /不支持的字段/
  );
  assert.throws(
    () =>
      model.edit(state, 'algo-located', {
        range: range(state, 1, 2),
        frequency: { lowerHz: 1, upperHz: 2 },
      }),
    /不能同时/
  );
  assert.deepEqual(model.save(state), original);
});

test('名称/备注的 undo 和 redo 均保留质量与 Current 几何关联', () => {
  const state = model.createState();
  model.select(state, 'algo-located');
  model.setQuality(state, 'Accepted');
  const originalName = model.active(state).name;
  model.edit(state, 'algo-located', {
    name: '撤销重做名称',
    note: '撤销重做备注',
  });
  assert.equal(relation(state), 'Current');
  model.undo(state);
  assert.equal(model.active(state).name, originalName);
  assert.equal(model.active(state).note, '');
  assert.equal(model.active(state).reviewStatus, 'Accepted');
  assert.equal(relation(state), 'Current');
  assertSelection(state);
  model.redo(state);
  assert.equal(model.active(state).name, '撤销重做名称');
  assert.equal(model.active(state).note, '撤销重做备注');
  assert.equal(model.active(state).reviewStatus, 'Accepted');
  assert.equal(relation(state), 'Current');
  assertSelection(state);
});

test('修改已绑定几何使质量 Pending、通道 Changed，undo/redo 恢复关联', () => {
  const state = model.createState();
  model.select(state, 'algo-located');
  model.setQuality(state, 'Accepted');
  const origin = clone(model.active(state).origin);
  model.edit(state, 'algo-located', { range: range(state, 3, 7) });
  assert.equal(model.active(state).reviewStatus, 'Pending');
  assert.equal(relation(state), 'Changed');
  assert.deepEqual(model.active(state).origin, origin);
  assert(model.undo(state));
  assert.equal(model.active(state).reviewStatus, 'Accepted');
  assert.equal(relation(state), 'Current');
  assertSelection(state);
  assert(model.redo(state));
  assert.equal(relation(state), 'Changed');
  assertSelection(state);
});

test('未绑定段的几何和新增独立段不改变第一段通道的关系', () => {
  const state = model.createState();
  model.select(state, 'algo-located', { segmentId: 'algo-located-s2' });
  model.edit(state, 'algo-located', {
    segmentId: 'algo-located-s2',
    range: range(state, 10, 16),
  });
  assert.equal(relation(state), 'Current');
  const added = model.addSegment(state, 'algo-located', range(state, 18, 19));
  assert.equal(model.active(state).activeSegmentId, added.id);
  assert.equal(relation(state), 'Current');
  assertSelection(state);
});

test('无段频率编辑保持观察时间，不产生算法段；已有段禁止伪装观察带', () => {
  const state = model.createState();
  const before = clone(model.active(state).observation);
  model.setQuality(state, 'Accepted');
  model.edit(state, 'algo-observation', {
    frequency: { lowerHz: 2499.5e6, upperHz: 2500e6 },
  });
  assert.equal(model.active(state).segments.length, 0);
  assert.equal(model.active(state).observation.startSample, before.startSample);
  assert.equal(model.active(state).observation.endSample, before.endSample);
  assert.equal(model.active(state).reviewStatus, 'Pending');
  assertSelection(state);
  assert.throws(
    () =>
      model.edit(state, 'algo-located', {
        frequency: { lowerHz: 2499e6, upperHz: 2500e6 },
      }),
    /没有活动段/
  );
  assert.throws(
    () => model.edit(state, 'algo-observation', { range: range(state, 1, 2) }),
    /不包含活动段/
  );
});

test('split 只在整数样本中点划分，保留左段 ID 和原始证据', () => {
  const state = model.createState();
  model.select(state, 'algo-located');
  const original = clone(model.segment(state));
  const origin = clone(model.active(state).origin);
  const parts = model.splitSegment(state, 'algo-located', original.id);
  assert.equal(parts.length, 2);
  assert.equal(parts[0].id, original.id);
  assert.equal(parts[0].range.startSample, original.range.startSample);
  assert.equal(parts[0].range.endSample, parts[1].range.startSample);
  assert.equal(parts[1].range.endSample, original.range.endSample);
  assert.equal(model.active(state).segments.length, 3);
  assert.deepEqual(model.active(state).origin, origin);
  assert.equal(relation(state), 'Changed');
  assertSelection(state);
  assert(model.undo(state));
  assert.equal(relation(state), 'Current');
});

test('单样本段不能拆分，极大安全整数样本仍使用精确中点', () => {
  const state = model.createState();
  const tiny = model.createManual(state, {
    startSample: 0,
    endSample: 1,
    frequencyLowerHz: 2499e6,
    frequencyUpperHz: 2500e6,
  });
  const undoCount = state.undo.length;
  assert.throws(
    () => model.splitSegment(state, tiny.id, tiny.activeSegmentId),
    /不足两个样本/
  );
  assert.equal(state.undo.length, undoCount);
  model.file(state).totalSamples = Number.MAX_SAFE_INTEGER;
  const large = model.createManual(state, {
    startSample: Number.MAX_SAFE_INTEGER - 3,
    endSample: Number.MAX_SAFE_INTEGER,
    frequencyLowerHz: 2499e6,
    frequencyUpperHz: 2500e6,
  });
  const [left, right] = model.splitSegment(
    state,
    large.id,
    large.activeSegmentId
  );
  assert.equal(left.range.endSample, Number.MAX_SAFE_INTEGER - 2);
  assert.equal(right.range.startSample, left.range.endSample);
  assertSelection(state);
});

test('merge 合并相邻物理包围范围并使被移除段绑定为 Missing', () => {
  const state = model.createState();
  model.select(state, 'algo-located', { segmentId: 'algo-located-s2' });
  const secondChannel = model.createChannel(
    state,
    model.defaultChannelRequest(state)
  );
  model.mergeAdjacent(state, 'algo-located', 'algo-located-s1');
  assert.equal(model.active(state).segments.length, 1);
  assert.deepEqual(model.segment(state).range, range(state, 2.9, 16.6));
  assert.equal(relation(state), 'Changed');
  assert.equal(
    model.channelRelation(model.file(state), secondChannel),
    'Missing'
  );
  assertSelection(state);
  assert.throws(
    () => model.mergeAdjacent(state, 'algo-located', 'algo-located-s1'),
    /只有一个/
  );
});

test('删除已绑定段保留 Missing 通道，删除最后一段软删整个对象且一次撤销', () => {
  const state = model.createState();
  model.select(state, 'algo-located');
  model.deleteSegment(state, 'algo-located', 'algo-located-s1');
  assert.equal(relation(state), 'Missing');
  assert.equal(model.active(state).segments.length, 1);
  const count = state.undo.length;
  model.deleteSegment(state, 'algo-located', 'algo-located-s2');
  assert.equal(state.undo.length, count + 1);
  assert.equal(find(state, 'algo-located').deleted, true);
  assert.equal(model.file(state).channels.length, 1);
  assert.equal(relation(state), 'Missing');
  assertSelection(state);
  model.undo(state);
  assert.equal(find(state, 'algo-located').deleted, false);
  assert.equal(model.active(state).segments.length, 1);
  model.undo(state);
  assert.equal(relation(state), 'Current');
  assertSelection(state);
});

test('恢复原始范围仅恢复几何，保留当前名称及来源，通道按实际几何恢复', () => {
  const state = model.createState();
  model.select(state, 'algo-located');
  model.edit(state, 'algo-located', {
    name: '保留改名',
    range: range(state, 3, 7),
  });
  model.restoreOriginRange(state, 'algo-located');
  assert.equal(model.active(state).name, '保留改名');
  assert.deepEqual(
    model.active(state).segments,
    model.active(state).origin.segments
  );
  assert.equal(model.active(state).sourceKind, 'Algorithm');
  assert.equal(model.active(state).reviewStatus, 'Pending');
  assert.equal(relation(state), 'Current');
  assertSelection(state);
});

test('新建人工标记记录独立 origin、Pending 与单次 undo，非法范围不改状态', () => {
  const state = model.createState();
  const requested = range(state, 1, 2, 2494, 2495);
  const created = model.createManual(state, requested);
  assert.equal(created.sourceKind, 'Manual');
  assert.equal(created.reviewStatus, 'Pending');
  assert.deepEqual(created.origin.segments[0].range, requested);
  assert.equal(state.undo.length, 1);
  assert.equal(model.active(state).id, created.id);
  assertSelection(state);
  model.undo(state);
  assert.equal(model.file(state).annotations.length, 8);
  const before = model.save(state);
  assert.throws(
    () => model.createManual(state, { ...requested, endSample: 400001 }),
    /物理样本/
  );
  assert.deepEqual(model.save(state), before);
});

test('批量复核是一个 undo，不改修订 ID、不改变几何通道关系', () => {
  const state = model.createState();
  model.select(state, 'algo-located');
  model.select(state, 'algo-ignored', { ctrlKey: true });
  const revisions = model
    .file(state)
    .selection.ids.map(id => find(state, id).revisionId);
  model.setQuality(state, 'Accepted');
  assert.equal(state.undo.length, 1);
  assert.deepEqual(
    model.file(state).selection.ids.map(id => find(state, id).revisionId),
    revisions
  );
  assert.equal(relation(state), 'Current');
  model.undo(state);
  assert.equal(find(state, 'algo-located').reviewStatus, 'Pending');
  assert.equal(find(state, 'algo-ignored').reviewStatus, 'Rejected');
  assertSelection(state);
  assert.throws(() => model.setQuality(state, 'Confirmed'), /不支持/);
});

test('批量软删除统计对象/段/关联通道，仅一次 undo 并保留 Missing 通道', () => {
  const state = model.createState();
  model.select(state, 'algo-located');
  model.select(state, 'manual-1', { ctrlKey: true });
  assert.deepEqual(model.deleteSummary(state), {
    objects: 2,
    segments: 4,
    channels: 1,
  });
  assert.deepEqual(model.deleteSelected(state), {
    objects: 2,
    segments: 4,
    channels: 1,
  });
  assert.equal(state.undo.length, 1);
  assert.equal(model.file(state).channels.length, 1);
  assert.equal(relation(state), 'Missing');
  assert.equal(model.active(state), null);
  model.undo(state);
  assert.equal(model.file(state).selection.ids.length, 2);
  assert.equal(relation(state), 'Current');
  assertSelection(state);
  model.redo(state);
  assert.equal(relation(state), 'Missing');
  assertSelection(state);
});

test('跨页批量删除覆盖所有匹配对象，undo 后按当前筛选协调选择', () => {
  const state = model.createState('large');
  model.selectAll(state);
  model.deleteSelected(state);
  assert.equal(model.visibleAnnotations(state).length, 0);
  assert.equal(state.undo.length, 1);
  model.applyFilter(state, { source: 'Algorithm' });
  model.undo(state);
  assert.equal(model.file(state).selection.ids.length, 263);
  model
    .file(state)
    .selection.ids.forEach(id =>
      assert.equal(find(state, id).sourceKind, 'Algorithm')
    );
  assertSelection(state);
});

test('质量筛选下复核移出选择，undo/redo 不复活隐藏选择', () => {
  const state = model.createState();
  model.applyFilter(state, { quality: 'Pending' });
  model.select(state, 'algo-located');
  model.setQuality(state, 'Accepted');
  assert.equal(model.active(state), null);
  model.applyFilter(state, { source: 'ImportedLabel' });
  model.undo(state);
  assert.equal(model.active(state), null);
  assertSelection(state);
  model.redo(state);
  assertSelection(state);
});

test('撤销后新分支清空 redo 且修订 ID 不复用，旧草稿仍被拒绝', () => {
  const state = model.createState();
  model.edit(state, 'algo-observation', { name: '分支 A' });
  const abandonedRevision = model.active(state).revisionId;
  model.undo(state);
  assert.equal(state.redo.length, 1);
  model.edit(state, 'algo-observation', { name: '分支 B' });
  assert.equal(state.redo.length, 0);
  assert.notEqual(model.active(state).revisionId, abandonedRevision);
  assert.throws(
    () =>
      model.edit(
        state,
        'algo-observation',
        { note: 'A 草稿' },
        abandonedRevision
      ),
    /过期/
  );
  assert.equal(model.redo(state), false);
});

test('ID 级 undo/redo 保留命令后后台发布的独立对象及通道', () => {
  const state = model.createState();
  const sourceFile = model.file(state);
  model.edit(state, 'algo-located', { note: '仅该 ID 被命令修改' });
  const published = publishSyntheticAnnotation(
    sourceFile,
    'background-published'
  );
  const publishedChannel = publishSyntheticChannel(
    sourceFile,
    published,
    'background-channel'
  );
  const publishedEvidence = clone(published);
  const channelEvidence = clone(publishedChannel);
  assert.equal(state.undo[0].changes.length, 1);
  assert.deepEqual(
    state.undo[0].changes[0].annotationChanges.map(change => change.id),
    ['algo-located']
  );
  assert.equal(state.undo[0].changes[0].channelChanges.length, 0);
  assert(!JSON.stringify(state.undo[0]).includes('"view"'));
  for (const operation of [model.undo, model.redo]) {
    assert(operation(state));
    assert.equal(model.file(state), sourceFile);
    assert.equal(
      sourceFile.annotations.find(annotation => annotation.id === published.id),
      published
    );
    assert.equal(
      sourceFile.channels.find(channel => channel.id === publishedChannel.id),
      publishedChannel
    );
    assert.deepEqual(published, publishedEvidence);
    assert.deepEqual(publishedChannel, channelEvidence);
    assertSelection(state);
  }
  assert.equal(find(state, 'algo-located').note, '仅该 ID 被命令修改');
});

test('命令后选中新发布对象，undo/redo 保留当前选择', () => {
  const state = model.createState();
  model.edit(state, 'algo-observation', { note: '修改原对象' });
  const published = publishSyntheticAnnotation(
    model.file(state),
    'background-selected'
  );
  model.select(state, published.id, { segmentId: published.segments[1].id });
  for (const operation of [model.undo, model.redo]) {
    operation(state);
    assert.equal(model.active(state).id, published.id);
    assert.equal(model.segment(state).id, published.segments[1].id);
    assertSelection(state);
  }
});

test('几何 undo/redo 保留当前图谱 view、源 metadata 与文件对象本身', () => {
  const state = model.createState();
  const sourceFile = model.file(state);
  model.select(state, 'algo-located');
  const originalGeometry = clone(model.segment(state).range);
  model.edit(state, 'algo-located', { range: range(state, 3, 7) });
  sourceFile.view = { startMs: 8, endMs: 11, lowerMHz: 2498, upperMHz: 2504 };
  sourceFile.sourceIdentity = 'background-source-v2';
  sourceFile.sourceAvailable = false;
  sourceFile.availableSamples = 200000;
  sourceFile.name = '后台更新的文件名称';
  const currentMetadata = clone({
    view: sourceFile.view,
    sourceIdentity: sourceFile.sourceIdentity,
    sourceAvailable: sourceFile.sourceAvailable,
    availableSamples: sourceFile.availableSamples,
    name: sourceFile.name,
  });
  model.undo(state);
  assert.deepEqual(
    find(state, 'algo-located').segments[0].range,
    originalGeometry
  );
  for (const operation of [null, model.redo]) {
    if (operation) operation(state);
    assert.equal(model.file(state), sourceFile);
    assert.deepEqual(
      {
        view: sourceFile.view,
        sourceIdentity: sourceFile.sourceIdentity,
        sourceAvailable: sourceFile.sourceAvailable,
        availableSamples: sourceFile.availableSamples,
        name: sourceFile.name,
      },
      currentMetadata
    );
    assert.equal(relation(state), 'Unavailable');
    assertSelection(state);
  }
});

test('跨文件 undo/redo 仅应用命令所触及记录，不切换当前文件或回滚其它文件', () => {
  const state = model.createState();
  const firstFile = model.file(state);
  model.edit(state, 'algo-observation', { note: 'file-a 的修改' });
  model.switchFile(state, 'file-b');
  const secondFile = model.file(state);
  const published = publishSyntheticAnnotation(secondFile, 'background-file-b');
  publishSyntheticChannel(secondFile, published, 'background-file-b-channel');
  model.select(state, published.id, { segmentId: published.segments[1].id });
  secondFile.view = { startMs: 1, endMs: 3, lowerMHz: 2490, upperMHz: 2510 };
  secondFile.sourceIdentity = 'background-file-b-v2';
  secondFile.sourceAvailable = false;
  const secondEvidence = clone(secondFile);
  model.undo(state);
  assert.equal(
    firstFile.annotations.find(
      annotation => annotation.id === 'algo-observation'
    ).note,
    ''
  );
  assert.equal(state.activeFileId, secondFile.id);
  assert.equal(model.file(state), secondFile);
  assert.deepEqual(secondFile, secondEvidence);
  model.redo(state);
  assert.equal(
    firstFile.annotations.find(
      annotation => annotation.id === 'algo-observation'
    ).note,
    'file-a 的修改'
  );
  assert.equal(state.activeFileId, secondFile.id);
  assert.deepEqual(secondFile, secondEvidence);
  assertSelection(state);
});

test('创建通道的 ID 级 undo/redo 保留后来发布通道，不回滚标记或视图', () => {
  const state = model.createState();
  const sourceFile = model.file(state);
  const created = model.createChannel(
    state,
    model.defaultChannelRequest(state)
  );
  const published = publishSyntheticAnnotation(
    sourceFile,
    'background-channel-owner'
  );
  const publishedChannel = publishSyntheticChannel(
    sourceFile,
    published,
    'background-extra-channel'
  );
  sourceFile.view = { ...sourceFile.view, startMs: 3, endMs: 5 };
  const currentView = clone(sourceFile.view);
  assert.equal(state.undo[0].changes[0].annotationChanges.length, 0);
  assert.deepEqual(
    state.undo[0].changes[0].channelChanges.map(change => change.id),
    [created.id]
  );
  model.undo(state);
  assert(!sourceFile.channels.some(channel => channel.id === created.id));
  assert(sourceFile.channels.includes(publishedChannel));
  assert(sourceFile.annotations.includes(published));
  assert.deepEqual(sourceFile.view, currentView);
  model.redo(state);
  assert(sourceFile.channels.some(channel => channel.id === created.id));
  assert(sourceFile.channels.includes(publishedChannel));
  assert.deepEqual(sourceFile.view, currentView);
});

test('批量软删除一条 ID 差异命令保留后续发布，undo/redo 不删除新对象', () => {
  const state = model.createState();
  model.select(state, 'algo-located');
  model.select(state, 'manual-1', { ctrlKey: true });
  model.deleteSelected(state);
  assert.equal(state.undo.length, 1);
  assert.equal(state.undo[0].changes[0].annotationChanges.length, 2);
  const published = publishSyntheticAnnotation(
    model.file(state),
    'background-after-delete'
  );
  model.undo(state);
  assert.equal(find(state, 'algo-located').deleted, false);
  assert.equal(find(state, 'manual-1').deleted, false);
  assert.equal(find(state, published.id), published);
  model.redo(state);
  assert.equal(find(state, 'algo-located').deleted, true);
  assert.equal(find(state, 'manual-1').deleted, true);
  assert.equal(find(state, published.id), published);
  assert.equal(published.deleted, false);
  assertSelection(state);
});

test('默认未知时间请求使用视图与可读前缀，不补算法段或改变复核', () => {
  const state = model.createState();
  const request = model.defaultChannelRequest(state);
  assert.equal(request.scopeOrigin, 'UserRange');
  assert.equal(request.segmentId, null);
  assert.equal(request.range.startSample, 0);
  assert.equal(request.range.endSample, 400000);
  assert.equal(request.referenceRange.endSample, 265536);
  assert.equal(request.outputSampleRateHz, 4e6);
  assert.deepEqual(model.validateChannelRequest(state, request), []);
  const created = model.createChannel(state, request);
  assert.equal(created.demoOnly, true);
  assert.equal(model.active(state).segments.length, 0);
  assert.equal(model.active(state).reviewStatus, 'Pending');
  assert.deepEqual(created.sourceSnapshot.range, request.range);
  assert.deepEqual(
    created.sourceSnapshot.referenceRange,
    request.referenceRange
  );
  assert.equal(relation(state, 1), 'Current');
  assert.equal(state.undo.length, 1);
});

test('Pending/Rejected 与 confirmed:false 均可创建通道，未额外强制确认', () => {
  const state = model.createState();
  model.select(state, 'algo-ignored');
  assert.equal(model.segment(state).confirmed, false);
  assert.equal(model.active(state).reviewStatus, 'Rejected');
  const channel = model.createChannel(
    state,
    model.defaultChannelRequest(state)
  );
  assert.equal(model.active(state).reviewStatus, 'Rejected');
  assert.equal(model.channelRelation(model.file(state), channel), 'Current');
  model.select(state, 'algo-located');
  assert.equal(model.active(state).reviewStatus, 'Pending');
  assert.doesNotThrow(() =>
    model.createChannel(state, model.defaultChannelRequest(state))
  );
});

test('无选择/多选择不生成默认请求；默认请求不改状态', () => {
  const state = model.createState();
  const before = model.save(state);
  model.defaultChannelRequest(state);
  assert.deepEqual(model.save(state), before);
  model.clearSelection(state);
  assert.throws(() => model.defaultChannelRequest(state), /只能选择一个/);
  model.select(state, 'manual-1');
  model.select(state, 'algo-located', { ctrlKey: true });
  assert.throws(() => model.defaultChannelRequest(state), /只能选择一个/);
});

test('明确段可裁剪，保留完整绑定几何与实际提取范围，越段被拒绝', () => {
  const state = model.createState();
  model.select(state, 'algo-located');
  const request = model.defaultChannelRequest(state);
  assert.equal(request.scopeOrigin, 'LocatedSegment');
  assert.deepEqual(request.range, model.segment(state).range);
  request.range = range(state, 4, 6, 2502.05, 2502.3);
  updateFrequency(request, 2502.05e6, 2502.3e6);
  assert.deepEqual(codes(state, request), []);
  const created = model.createChannel(state, request);
  assert.equal(created.range.startSample, 80000);
  assert.equal(created.sourceSnapshot.referenceRange.startSample, 58000);
  assert.equal(model.channelRelation(model.file(state), created), 'Current');
  request.range.endSample = 160000;
  assert(codes(state, request).includes('OutsideSegment'));
});

test('未知时间手工范围可超出原观察支持，保存 UserRange 不伪造 segment', () => {
  const state = model.createState();
  const request = model.defaultChannelRequest(state);
  request.range.startSample = 280000;
  request.range.endSample = 380000;
  assert.deepEqual(codes(state, request), []);
  const created = model.createChannel(state, request);
  assert.equal(created.segmentId, null);
  assert.equal(created.scopeOrigin, 'UserRange');
  assert.equal(created.range.endSample, 380000);
  assert.equal(created.sourceSnapshot.referenceRange.endSample, 265536);
  assert.equal(model.active(state).segments.length, 0);
});

test('partial 默认未知时间只到 12ms，明确段越可读前缀不会静默裁剪', () => {
  const state = model.createState('partial');
  const request = model.defaultChannelRequest(state);
  assert.equal(request.range.endSample, 240000);
  assert.deepEqual(codes(state, request), []);
  model.select(state, 'algo-located', { segmentId: 'algo-located-s2' });
  const located = model.defaultChannelRequest(state);
  assert.equal(located.range.endSample, 332000);
  assert(codes(state, located).includes('UnreadableRange'));
  located.range.endSample = 240000;
  assert.deepEqual(codes(state, located), []);
});

test('outside 保持空请求可打开对话框，create 拒绝空提取且不回退其它范围', () => {
  const state = model.createState('outside');
  const before = model.save(state);
  const request = model.defaultChannelRequest(state);
  assert.equal(request.range.startSample, request.range.endSample);
  assert(codes(state, request).includes('EmptyRange'));
  assert.throws(() => model.createChannel(state, request), /没有非空交集/);
  assert.deepEqual(model.save(state), before);
  assert.equal(state.undo.length, 0);
  request.range.startSample = 140000;
  request.range.endSample = 200000;
  assert.deepEqual(codes(state, request), []);
});

test('缺失/变化物理源保留请求并显示具体错误，创建失败为原子操作', () => {
  for (const [scenario, code] of [
    ['missing-source', 'SourceMissing'],
    ['changed-source', 'SourceChanged'],
  ]) {
    const state = model.createState(scenario);
    const before = model.save(state);
    const request = model.defaultChannelRequest(state);
    assert(codes(state, request).includes(code));
    assert.equal(relation(state), 'Unavailable');
    assert.throws(() => model.createChannel(state, request), /物理源/);
    assert.deepEqual(model.save(state), before);
    assert.equal(state.undo.length, 0);
  }
});

test('通道校验覆盖样本整数、非空、物理边界及可读前缀', () => {
  const state = model.createState('partial');
  const original = model.defaultChannelRequest(state);
  for (const [patch, expected] of [
    [{ startSample: 0.5 }, 'SampleInteger'],
    [{ startSample: 100, endSample: 100 }, 'EmptyRange'],
    [{ startSample: -1 }, 'PhysicalBounds'],
    [{ endSample: 400001 }, 'PhysicalBounds'],
    [{ endSample: 240001 }, 'UnreadableRange'],
  ]) {
    const request = clone(original);
    Object.assign(request.range, patch);
    assert(codes(state, request).includes(expected), expected);
  }
});

test('频率和 15% 保护带校验不悄改输入，不添加算法限制', () => {
  const state = model.createState();
  const request = model.defaultChannelRequest(state);
  updateFrequency(request, 2490e6, 2491e6);
  const before = clone(request);
  assert(codes(state, request).includes('FilterGuard'));
  assert.deepEqual(request, before);
  updateFrequency(request, 2498e6, 2501e6);
  request.outputSampleRateHz = 3.9e6;
  assert.deepEqual(codes(state, request), []);
  request.bandwidthHz = 2e6;
  assert(codes(state, request).includes('FrequencyMismatch'));
  request.range.frequencyUpperHz = request.range.frequencyLowerHz;
  assert(codes(state, request).includes('FrequencyRange'));
});

test('输出率边界允许 1.3BW 精确值，拒绝低于它或高于输入 Fs', () => {
  const state = model.createState();
  const request = model.defaultChannelRequest(state);
  request.outputSampleRateHz = 1.3 * request.bandwidthHz;
  assert.deepEqual(codes(state, request), []);
  request.outputSampleRateHz -= 1;
  assert(codes(state, request).includes('OutputRateLow'));
  request.outputSampleRateHz = 20e6 + 1;
  assert(codes(state, request).includes('OutputRateHigh'));
  request.outputSampleRateHz = 0;
  assert(codes(state, request).includes('OutputRateInvalid'));
  request.filter = '伪模型滤波';
  assert(codes(state, request).includes('FilterInvalid'));
});

test('通道默认输出率遵守 max(4MHz,1.3BW) 并保留无法满足的几何错误', () => {
  const state = model.createState();
  model.edit(state, 'algo-observation', {
    frequency: { lowerHz: 2495e6, upperHz: 2505e6 },
  });
  const request = model.defaultChannelRequest(state);
  assert.equal(request.outputSampleRateHz, 13e6);
  assert.deepEqual(codes(state, request), []);
  model.edit(state, 'algo-observation', {
    frequency: { lowerHz: 2490e6, upperHz: 2510e6 },
  });
  const broad = model.defaultChannelRequest(state);
  assert.equal(broad.outputSampleRateHz, 20e6);
  assert(codes(state, broad).includes('FilterGuard'));
  assert(codes(state, broad).includes('OutputRateLow'));
  assert.equal(broad.range.frequencyLowerHz, 2490e6);
});

test('通道请求绑定几何过期会拒绝；只改 metadata 不造成伪 Changed', () => {
  const state = model.createState();
  model.select(state, 'algo-located');
  const request = model.defaultChannelRequest(state);
  model.edit(state, 'algo-located', { note: '仅备注' });
  assert.deepEqual(codes(state, request), []);
  const created = model.createChannel(state, request);
  assert.equal(model.channelRelation(model.file(state), created), 'Current');
  model.edit(state, 'algo-located', { range: range(state, 3, 7) });
  assert(codes(state, request).includes('BindingChanged'));
});

test('UserRange 观察带频率/结构变更为 Changed，恢复原始范围可回到 Current', () => {
  const state = model.createState();
  const created = model.createChannel(
    state,
    model.defaultChannelRequest(state)
  );
  model.setQuality(state, 'Rejected');
  assert.equal(model.channelRelation(model.file(state), created), 'Current');
  model.edit(state, 'algo-observation', {
    frequency: { lowerHz: 2499.5e6, upperHz: 2500e6 },
  });
  assert.equal(model.channelRelation(model.file(state), created), 'Changed');
  model.restoreOriginRange(state, 'algo-observation');
  assert.equal(model.channelRelation(model.file(state), created), 'Current');
  model.addSegment(
    state,
    'algo-observation',
    range(state, 2, 3, 2499.38, 2500.12)
  );
  assert.equal(model.channelRelation(model.file(state), created), 'Changed');
  model.restoreOriginRange(state, 'algo-observation');
  assert.equal(model.channelRelation(model.file(state), created), 'Current');
  assert.equal(model.active(state).segments.length, 0);
  assertSelection(state);
});

test('四种通道关联互斥：Current/Changed/Missing/Unavailable', () => {
  const state = model.createState();
  model.select(state, 'algo-located');
  assert.equal(relation(state), 'Current');
  model.edit(state, 'algo-located', { range: range(state, 3, 7) });
  assert.equal(relation(state), 'Changed');
  model.file(state).sourceAvailable = false;
  assert.equal(relation(state), 'Unavailable');
  model.deleteSelected(state);
  assert.equal(relation(state), 'Missing');
});

test('保存去除历史栈和 hover，加载恢复演示状态并清空 undo/redo', () => {
  const state = model.createState();
  model.file(state).selection.hoverId = 'label-1';
  model.file(state).selection.hoverSegmentId = 'label-1-s1';
  model.edit(state, 'algo-observation', { note: '保存备注' });
  const saved = model.save(state);
  assert(!('undo' in saved));
  assert(!('redo' in saved));
  assert(!('hoverId' in saved.files[0].selection));
  assert(!('hoverSegmentId' in saved.files[0].selection));
  assert.deepEqual(model.snapshot(state), saved);
  const loaded = model.load(JSON.stringify(saved));
  assert.equal(loaded.undo.length, 0);
  assert.equal(loaded.redo.length, 0);
  assert.equal(model.file(loaded).selection.hoverId, null);
  assert.equal(model.active(loaded).note, '保存备注');
  assert.deepEqual(model.save(loaded), saved);
  assertSelection(loaded);
});

test('保存加载支持未确认 UserRange 通道与软删对象保留的 Missing 通道', () => {
  const state = model.createState();
  const created = model.createChannel(
    state,
    model.defaultChannelRequest(state)
  );
  model.deleteSelected(state);
  const loaded = model.load(model.save(state));
  const channel = model
    .file(loaded)
    .channels.find(value => value.id === created.id);
  assert.equal(channel.scopeOrigin, 'UserRange');
  assert.equal(channel.segmentId, null);
  assert.equal(model.channelRelation(model.file(loaded), channel), 'Missing');
  assert.equal(find(loaded, 'algo-observation').segments.length, 0);
  assert.equal(find(loaded, 'algo-observation').deleted, true);
  assert.equal(model.file(loaded).channels.length, 2);
});

test('保存加载允许段已删除的 Missing 绑定和源变化/缺失的 Unavailable 绑定', () => {
  const state = model.createState();
  model.deleteSegment(state, 'algo-located', 'algo-located-s1');
  assert.equal(relation(model.load(model.save(state))), 'Missing');
  for (const scenario of ['changed-source', 'missing-source']) {
    assert.equal(
      relation(model.load(model.save(model.createState(scenario)))),
      'Unavailable'
    );
  }
});

test('加载验证 schema/version/demoOnly、物理长度及重复 ID，拒绝伪造状态', () => {
  const state = model.createState();
  for (const [change, message] of [
    [
      saved => {
        saved.schema = 'native-project';
      },
      /schema/,
    ],
    [
      saved => {
        saved.version = 5;
      },
      /version 6/,
    ],
    [
      saved => {
        saved.demoOnly = false;
      },
      /demoOnly/,
    ],
    [
      saved => {
        saved.files[0].availableSamples = 400001;
      },
      /可读前缀/,
    ],
    [
      saved => {
        saved.files[0].annotations.push(clone(saved.files[0].annotations[0]));
      },
      /重复标识/,
    ],
    [
      saved => {
        saved.files[0].annotations[0].segments[0].range.startSample = 0.2;
      },
      /安全整数/,
    ],
    [
      saved => {
        saved.files[0].annotations[0].origin = null;
      },
      /原始证据/,
    ],
    [
      saved => {
        saved.files[0].annotations[0].reviewStatus = 'Confirmed';
      },
      /属性无效/,
    ],
    [
      saved => {
        saved.files[0].channels[0].range.endSample += 1;
      },
      /创建快照不一致/,
    ],
  ]) {
    const saved = model.save(state);
    change(saved);
    assert.throws(() => model.load(saved), message);
  }
  assert.throws(() => model.load('{'), /JSON/);
  assert.throws(() => model.load(null), /schema/);
});

test('加载先验证完整数据，失败不触碰既有工作状态', () => {
  const state = model.createState();
  const before = model.save(state);
  const invalid = clone(before);
  invalid.files[1].annotations = [clone(invalid.files[0].annotations[0])];
  invalid.files[1].annotations[0].segments[0].range.endSample = 900000;
  assert.throws(() => model.load(invalid), /物理样本/);
  assert.deepEqual(model.save(state), before);
});

test('加载按全局筛选协调每个文件的选择，并保持有效 active/segment', () => {
  const state = model.createState();
  model.select(state, 'manual-1', { segmentId: 'manual-1-s2' });
  const saved = model.save(state);
  saved.filters.layers.Manual = false;
  saved.files[0].selection.ids.push('不存在对象');
  const loaded = model.load(saved);
  assert.equal(model.active(loaded), null);
  assertSelection(loaded);
  assert.equal(model.file(loaded).selection.hoverSegmentId, null);
});

test('所有规定场景均可独立 JSON 往返', () => {
  for (const scenario of [
    'standard',
    'large',
    'partial',
    'outside',
    'changed-source',
    'missing-source',
  ]) {
    const state = model.createState(scenario);
    const restored = model.load(JSON.stringify(model.save(state)));
    assert.equal(restored.scenario, scenario);
    assert.equal(restored.activeFileId, state.activeFileId);
    assert.equal(
      model.visibleAnnotations(restored).length,
      model.visibleAnnotations(state).length
    );
    assertSelection(restored);
  }
});

test('瀑布显示模式可独立保存加载，不更改物理范围或通道关联', () => {
  const state = model.createState();
  const originalView = clone(model.file(state).view);
  state.mode = 'waterfall';
  const loaded = model.load(model.save(state));
  assert.equal(loaded.mode, 'waterfall');
  assert.deepEqual(model.file(loaded).view, originalView);
  assert.equal(relation(loaded), 'Current');
});

const failed = results.filter(result => !result.passed);
const outputUrl = new URL(
  '../out/tasks/wideband-detection/U0/diagnostics/model-r1.json',
  import.meta.url
);
const summary = {
  schema: 'signal-studio-u0-model-behavior-report',
  version: 1,
  demoOnly: true,
  evidenceBoundary:
    '仅独立纯状态/命令行为；没有读取 IQ、执行 DSP/模型、Qt 或 GPU 验证。',
  startedAt,
  completedAt: new Date().toISOString(),
  node: process.version,
  passed: results.length - failed.length,
  failed: failed.length,
  total: results.length,
  tests: results,
};
await mkdir(new URL('.', outputUrl), { recursive: true });
await writeFile(outputUrl, `${JSON.stringify(summary, null, 2)}\n`, 'utf8');
console.log(
  `U0 model: ${summary.passed}/${summary.total} passed, ${summary.failed} failed.`
);
console.log(fileURLToPath(outputUrl));
if (failed.length) {
  failed.forEach(result =>
    console.error(`${result.name}\n${result.stack ?? result.message}`)
  );
  process.exitCode = 1;
}
