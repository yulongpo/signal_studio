// Browser-only interaction checks. This never launches Qt or a detection model.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath, pathToFileURL } from 'node:url';

const require = createRequire(import.meta.url);
const { chromium } = require(process.env.SS_PLAYWRIGHT_MODULE || 'playwright');
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const output = path.join(root, 'docs/implementation/wideband-detection/M0-P');
fs.mkdirSync(path.join(output, 'screenshots'), { recursive: true });
const url = process.env.SS_PROTOTYPE_URL || pathToFileURL(path.join(root, 'docs/prototype/wideband-detection-r1/index.html')).href;
const browser = await chromium.launch({ channel: 'msedge', headless: true });
const page = await browser.newPage({ viewport: { width: 2560, height: 1440 }, deviceScaleFactor: 1 });
page.setDefaultTimeout(10000);
const errors = [];
page.on('pageerror', error => errors.push(error.message));
const checks = [];
const settle = () => page.waitForTimeout(100);
const snapshot = () => page.evaluate(() => window.signalStudioPrototype.snapshot());
const activeFile = state => state.files.find(file => file.id === state.activeFileId);
async function scene(value = 'S1') {
  await page.selectOption('#scenario', value);
  await settle();
}
async function clickAction(action) {
  await page.locator(`[data-action="${action}"]:visible`).first().click();
  await settle();
}
async function check(name, action) {
  try {
    await scene();
    await action();
    checks.push({ name, status: 'passed' });
    console.log(`PASS ${name}`);
  } catch (error) {
    checks.push({ name, status: 'failed', error: error.message });
    console.log(`FAIL ${name}: ${error.message.slice(0, 220)}`);
    await page.screenshot({ path: path.join(output, 'screenshots', `failure-${checks.length}.png`) });
  }
}
async function dragPlot(a = [.24, .25], b = [.68, .73]) {
  const box = await page.locator('#specCanvas').boundingBox();
  assert.ok(box);
  await page.mouse.move(box.x + box.width * a[0], box.y + box.height * a[1]);
  await page.mouse.down();
  await page.mouse.move(box.x + box.width * b[0], box.y + box.height * b[1], { steps: 8 });
  await page.mouse.up();
  await settle();
}
async function selectAnnotation(id) {
  await page.locator('[data-tab="annotations"]').click();
  await page.locator(`#annotationRows [data-annotation="${id}"]`).click();
  await settle();
}
async function editStart(id, deltaMs = .1) {
  const state = await snapshot();
  const file = activeFile(state);
  const annotation = file.annotations.find(a => a.id === id);
  await page.locator('#annotationDetail [data-geometry="t0"]').fill(String(Number(annotation.segments[0].range.startSample) / file.sampleRateHz * 1000 + deltaMs));
  await page.locator('#annotationDetail [data-geometry="t0"]').press('Enter');
  await settle();
}
async function markPoint(range, u, v) {
  const state = await snapshot(), file = activeFile(state);
  const box = await page.locator('#specCanvas').boundingBox();
  const t = (Number(range.startSample) + (Number(range.endSample) - Number(range.startSample)) * u) / file.sampleRateHz;
  const f = range.frequencyUpperHz - (range.frequencyUpperHz - range.frequencyLowerHz) * v;
  return { x: box.x + 77 + (t - file.view.t0) / (file.view.t1 - file.view.t0) * (box.width - 102),
    y: box.y + 13 + (file.view.f1 - f) / (file.view.f1 - file.view.f0) * (box.height - 46) };
}
async function dragElement(selector, dx, dy) {
  const box = await page.locator(selector).boundingBox();
  assert.ok(box);
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  await page.mouse.down();
  await page.mouse.move(box.x + box.width / 2 + dx, box.y + box.height / 2 + dy, { steps: 5 });
  await page.mouse.up();
  await settle();
}

try {
  await page.goto(url);
  await page.waitForFunction(() => Boolean(window.signalStudioPrototype));
  await settle();
  await page.screenshot({ path: path.join(output, 'screenshots', 'ready.png') });
  await check('Bootstrap and target layout', async () => {
    assert.equal(errors.length, 0);
    const overflow = await page.evaluate(() => document.documentElement.scrollWidth > innerWidth);
    assert.equal(overflow, false);
    assert.match(await page.locator('body').innerText(), /合成|演示/);
    assert.ok(activeFile(await snapshot()).annotations.some(a => a.sourceKind === 'ImportedLabel'));
    assert.ok(activeFile(await snapshot()).annotations.some(a => a.sourceKind === 'Manual'));
    assert.ok(activeFile(await snapshot()).annotations.some(a => a.sourceKind === 'Algorithm'));
  });
  await check('I01 three detection entries do not start a run', async () => {
    const count = (await snapshot()).runs.length;
    await page.locator('[data-menu="analysisMenu"]').click();
    await page.locator('#menuDetect').click();
    await page.locator('#scopeDetect').click();
    const box = await page.locator('#specCanvas').boundingBox();
    await page.mouse.click(box.x + box.width * .5, box.y + box.height * .5, { button: 'right' });
    await page.locator('#contextMenu [data-action="detect"]').click();
    assert.equal((await snapshot()).runs.length, count);
  });
  await check('I03 ROI cancellation restores continuous mark tool', async () => {
    await page.locator('#manualTool').click();
    const before = activeFile(await snapshot()).annotations.length;
    await page.locator('#roiTool').click();
    await dragPlot();
    assert.ok((await snapshot()).roiDraft);
    await page.keyboard.press('Escape');
    const state = await snapshot();
    assert.equal(state.tool, 'manual');
    assert.equal(activeFile(state).annotations.length, before);
    assert.equal(state.roiDraft, null);
  });
  await check('I04 ROI confirmation is separate from marking', async () => {
    const before = activeFile(await snapshot()).annotations.length;
    await page.locator('#roiTool').click();
    await dragPlot();
    await page.screenshot({ path: path.join(output, 'screenshots', 'roi-confirm.png') });
    await page.keyboard.press('Enter');
    const state = await snapshot();
    assert.ok(state.roi);
    assert.equal(state.roiDraft, null);
    assert.equal(activeFile(state).annotations.length, before);
    assert.equal(state.runs.length, 0);
  });
  await check('I04 ROI is clipped at usable source boundaries', async () => {
    await scene('S2Partial');
    await page.locator('#roiTool').click();
    await dragPlot([.3, .35], [1.1, -.1]);
    await page.keyboard.press('Enter');
    const state = await snapshot(), file = activeFile(state);
    assert.ok(state.roi);
    assert.ok(Number(state.roi.endSample) <= file.availableSamples);
    assert.ok(Number(state.roi.startSample) >= 0);
    assert.ok(state.roi.frequencyLowerHz >= file.centerFrequencyHz - file.sampleRateHz / 2);
    assert.ok(state.roi.frequencyUpperHz <= file.centerFrequencyHz + file.sampleRateHz / 2);
  });
  await check('I06 mode switch preserves physical records', async () => {
    const before = activeFile(await snapshot()).annotations;
    await selectAnnotation('manual-1');
    const selected = (await snapshot()).selectedIds;
    await page.locator('[data-mode="waterfall"]').click();
    await settle();
    assert.equal((await snapshot()).mode, 'waterfall');
    assert.deepEqual(activeFile(await snapshot()).annotations, before);
    assert.deepEqual((await snapshot()).selectedIds, selected);
    await page.selectOption('#frequencyMode', 'Baseband');
    assert.equal((await snapshot()).frequencyMode, 'Baseband');
    assert.deepEqual(activeFile(await snapshot()).annotations, before);
    await page.locator('[data-mode="spectrogram"]').click();
  });
  await check('I10 accepting twice keeps one source object', async () => {
    const before = activeFile(await snapshot()).annotations.length;
    await page.locator('[data-annotation="algo-sample"] [data-check]').check();
    await page.locator('#batchAccept').click();
    await settle();
    await page.locator('#batchAccept').click();
    const state = await snapshot();
    const annotation = activeFile(state).annotations.find(a => a.id === 'algo-sample');
    assert.equal(annotation.sourceKind, 'Algorithm');
    assert.equal(annotation.reviewStatus, 'Accepted');
    assert.equal(activeFile(state).annotations.length, before);
    assert.equal(activeFile(state).annotations.filter(a => a.id === 'algo-sample').length, 1);
  });
  await check('I11 source filter and visible-only select all', async () => {
    await page.selectOption('#filterSource', 'Algorithm');
    await settle();
    await page.locator('#selectAll').check();
    await settle();
    const state = await snapshot();
    assert.ok(state.selectedIds.length > 0);
    assert.ok(state.selectedIds.every(id => activeFile(state).annotations.find(a => a.id === id).sourceKind === 'Algorithm'));
  });
  await check('I11 score sorting and batch review preserve unselected records', async () => {
    const before = activeFile(await snapshot()).annotations;
    await page.locator('#filterScore').fill('0.85');
    await page.locator('#filterScore').press('Tab');
    await page.locator('th[data-sort="score"]').click();
    await settle();
    const visible = (await snapshot()).derived.visibleIds;
    assert.ok(visible.length > 1);
    const scores = visible.map(id => before.find(a => a.id === id).originEvidence.scoreMean);
    assert.ok(scores.every((v, i) => v >= .85 && (i === 0 || v >= scores[i - 1])));
    await page.locator('#selectAll').check();
    await page.locator('#batchReject').click();
    const after = activeFile(await snapshot()).annotations;
    for (const a of after) {
      if (visible.includes(a.id)) assert.equal(a.reviewStatus, 'Rejected');
      else assert.deepEqual(a, before.find(x => x.id === a.id));
    }
  });
  await check('I12 layer visibility does not mutate stored records', async () => {
    const before = activeFile(await snapshot()).annotations;
    await page.locator('[data-layer="ImportedLabel"]').uncheck();
    await settle();
    assert.deepEqual(activeFile(await snapshot()).annotations, before);
  });
  await check('I13 precision strategy enables only effective parameters', async () => {
    await page.locator('[data-strategy="coarse"]').click();
    await settle();
    assert.equal((await snapshot()).params.strategy, 'coarse');
    assert.equal(await page.locator('#minOnMs').isDisabled(), true);
    assert.equal(await page.locator('#minBurstMs').isDisabled(), false);
    await page.locator('[data-strategy="roi"]').click();
    await settle();
    assert.equal(await page.locator('#minBurstMs').isDisabled(), true);
    await page.locator('#splitEnabled').uncheck();
    assert.equal(await page.locator('#minOnMs').isDisabled(), true);
    assert.equal(await page.locator('#minOffMs').isDisabled(), true);
    await page.locator('[data-strategy="coarse"]').click();
    assert.equal(await page.locator('#splitGapMs').isDisabled(), true);
    assert.equal(await page.locator('#minBurstMs').isDisabled(), true);
  });
  await check('I14 parameter drafts survive refresh until Enter', async () => {
    await page.locator('#score').fill('0.8');
    assert.equal((await snapshot()).params.score, .7);
    await page.evaluate(() => window.signalStudioPrototype.dispatch('refresh'));
    await settle();
    assert.equal(await page.locator('#score').inputValue(), '0.8');
    await page.locator('#score').press('Enter');
    assert.equal((await snapshot()).params.score, .8);
  });
  await check('I14 blur preserves drafts and Tab/ShiftTab/click commit', async () => {
    const input = page.locator('#score');
    await input.fill('0.84');
    await page.evaluate(() => window.dispatchEvent(new Event('blur')));
    await settle();
    assert.equal((await snapshot()).params.score, .7);
    assert.equal(await input.inputValue(), '0.84');
    await input.press('Tab');
    assert.equal((await snapshot()).params.score, .84);
    await input.fill('0.83');
    await input.press('Shift+Tab');
    assert.equal((await snapshot()).params.score, .83);
    await input.fill('0.82');
    await page.locator('#scopeDetect').click();
    assert.equal((await snapshot()).params.score, .82);
  });
  await check('I14 invalid shape/hop stays draft and effective hop round-trips', async () => {
    await page.locator('#modelHopFrames').fill('11');
    await page.locator('#modelHopFrames').press('Enter');
    assert.equal((await snapshot()).params.modelHopFrames, 5);
    await page.locator('#runButton').click();
    assert.equal((await snapshot()).runs.length, 0);
    await page.locator('#modelHopFrames').fill('5');
    await page.locator('#modelHopFrames').press('Enter');
    await page.locator('#hopMs').fill('0.000001');
    await page.locator('#hopMs').press('Enter');
    assert.equal((await snapshot()).params.hopMs, 1);
    await page.locator('#runButton').click();
    assert.equal((await snapshot()).runs.length, 0);
    await page.locator('#hopMs').fill('0.1666667');
    await page.locator('#hopMs').press('Enter');
    await page.locator('#runButton').click();
    const state = await snapshot(), file = activeFile(state), run = state.runs.at(-1);
    assert.ok(run);
    const effectiveHop = Math.floor(file.sampleRateHz * run.snapshot.params.hopMs / 1000) / file.sampleRateHz * 1000;
    assert.equal(run.snapshot.timing.hopMs, effectiveHop);
    const durationMs = (Number(run.snapshot.contextRange.endSample) - Number(run.snapshot.contextRange.startSample)) / file.sampleRateHz * 1000;
    assert.equal(run.counts.totalSpectra, Math.floor((durationMs - run.snapshot.timing.fftMs) / effectiveHop) + 1);
    await clickAction('save');
    assert.match(await page.locator('#toast').innerText(), /已保存/);
    await scene('S0');
    await page.locator('[data-menu="fileMenu"]').click();
    await page.locator('#fileMenu [data-action="open-project"]').click();
    await settle();
    assert.equal((await snapshot()).params.hopMs, .1666667);
  });
  await check('I15 wheel does not commit parameter changes', async () => {
    const before = (await snapshot()).params;
    await page.locator('#score').hover();
    await page.mouse.wheel(0, -480);
    await settle();
    assert.deepEqual((await snapshot()).params, before);
    await page.locator('#profile').hover();
    const top = await page.locator('.inspector-scroll').evaluate(el => el.scrollTop);
    await page.mouse.wheel(0, 240);
    await settle();
    assert.deepEqual((await snapshot()).params, before);
    assert.ok(await page.locator('.inspector-scroll').evaluate(el => el.scrollTop) >= top);
  });
  await check('I16 cancellation keeps published partial results', async () => {
    await scene('S5');
    for (let i = 0; i < 3; i++) { await page.locator('#advanceRun').click(); await settle(); }
    const before = activeFile(await snapshot()).annotations.length;
    await page.screenshot({ path: path.join(output, 'screenshots', 'running-frozen.png') });
    await page.locator('#cancelRun').click();
    await settle();
    assert.equal((await snapshot()).runs.at(-1).state, 'cancelRequested');
    await page.locator('#advanceRun').click();
    await settle();
    const state = await snapshot();
    assert.equal(state.runs.at(-1).state, 'cancelled');
    assert.equal(activeFile(state).annotations.length, before);
    await page.screenshot({ path: path.join(output, 'screenshots', 'cancelled-partial.png') });
  });
  await check('I08 undo preserves subsequently published run results', async () => {
    await selectAnnotation('manual-1');
    await page.locator('#batchReject').click();
    await page.locator('#runButton').click();
    for (let i = 0; i < 3; i++) { await page.locator('#advanceRun').click(); await settle(); }
    const state = await snapshot();
    const publishedIds = state.runs.at(-1).outputs;
    assert.ok(publishedIds.length > 0);
    const published = activeFile(state).annotations.filter(a => publishedIds.includes(a.id));
    await page.locator('#undoButton').click();
    await settle();
    const restored = activeFile(await snapshot());
    assert.deepEqual(restored.annotations.filter(a => publishedIds.includes(a.id)), published);
    assert.equal(restored.annotations.find(a => a.id === 'manual-1').reviewStatus, 'Accepted');
    assert.ok((await snapshot()).runs.at(-1).outputs.every(id => restored.annotations.some(a => a.id === id)));
  });
  await check('S0-S9 scenarios remain distinct', async () => {
    for (const value of ['S0', 'S1', 'S2Loading', 'S2Partial', 'S3', 'S4Model', 'S4Short', 'S5', 'S6Cancel', 'S6Failed', 'S6Partial', 'S7', 'S7Zero', 'S8', 'S9']) {
      await scene(value);
      assert.equal((await snapshot()).scenario, value);
      if (value === 'S4Model' || value === 'S4Short' || value === 'S0') assert.equal(await page.locator('#runButton').isDisabled(), true);
    }
  });
  await check('I18 unknown-time evidence remains an observation', async () => {
    await page.locator('#annotationRows [data-annotation="algo-window"]').click();
    await settle();
    const annotation = activeFile(await snapshot()).annotations.find(a => a.id === 'algo-window');
    assert.equal(annotation.segments.length, 0);
    assert.ok(annotation.observation);
    assert.match(await page.locator('#annotationRows [data-annotation="algo-window"]').innerText(), /时间未定位/);
    assert.equal(await page.locator('#channelButton').isDisabled(), true);
    for (const id of ['algo-coarse', 'algo-sample', 'algo-roi', 'algo-fallback', 'manual-1']) {
      await selectAnnotation(id);
      assert.ok((await page.locator('#annotationDetail').innerText()).length > 100);
      assert.ok(activeFile(await snapshot()).annotations.find(a => a.id === id).originEvidence.refineMethod);
    }
  });
  await check('Profile17 keeps ten real time frames', async () => {
    await page.selectOption('#profile', 'ffscn17');
    await page.locator('#profile').press('Tab');
    await settle();
    assert.equal((await snapshot()).params.modelFrames, 10);
    assert.match(await page.locator('#profileHelp').innerText(), /实验/);
  });
  await check('I13 committed parameters stay with their own profile', async () => {
    await page.locator('#score').fill('0.81');
    await page.locator('#score').press('Enter');
    await page.selectOption('#profile', 'scn');
    await page.locator('#profile').press('Tab');
    await page.locator('#score').fill('0.63');
    await page.locator('#score').press('Enter');
    assert.equal((await snapshot()).params.modelFrames, 16);
    await page.selectOption('#profile', 'ffscn');
    await page.locator('#profile').press('Tab');
    assert.equal((await snapshot()).params.score, .81);
    assert.equal((await snapshot()).params.modelFrames, 10);
    await page.selectOption('#profile', 'scn');
    await page.locator('#profile').press('Tab');
    assert.equal((await snapshot()).params.score, .63);
  });
  await check('I02 frozen run survives background zoom', async () => {
    await scene('S5');
    const request = (await snapshot()).runs.at(-1).snapshot;
    await dragPlot([.15, .15], [.70, .40]);
    assert.deepEqual((await snapshot()).runs.at(-1).snapshot, request);
    assert.ok(request.contextRange);
  });
  await check('I02 short ROI borrows bounded real-frame context', async () => {
    await page.locator('#roiTool').click();
    await dragPlot([.30, .18], [.38, .52]);
    await page.keyboard.press('Enter');
    await page.locator('#runButton').click();
    const state = await snapshot();
    const run = state.runs.at(-1);
    assert.ok(run);
    const file = activeFile(state);
    assert.ok(Number(run.snapshot.contextRange.endSample) - Number(run.snapshot.contextRange.startSample) > Number(run.snapshot.range.endSample) - Number(run.snapshot.range.startSample));
    assert.ok(Number(run.snapshot.contextRange.startSample) >= 0);
    assert.ok(Number(run.snapshot.contextRange.endSample) <= file.availableSamples);
    assert.equal(run.snapshot.params.modelFrames, 10);
  });
  await check('I05 box zoom and wheel remain within source bounds', async () => {
    const initial = activeFile(await snapshot()).view;
    await dragPlot([.15, .15], [.70, .40]);
    let file = activeFile(await snapshot());
    assert.ok(file.view.t1 - file.view.t0 < initial.t1 - initial.t0);
    const box = await page.locator('#specCanvas').boundingBox();
    await page.mouse.move(box.x + box.width * .5, box.y + box.height - 15);
    await page.mouse.wheel(0, 2000);
    await settle();
    file = activeFile(await snapshot());
    assert.ok(file.view.t0 >= 0 && file.view.t1 <= file.availableSamples / file.sampleRateHz);
    assert.ok(file.view.f0 >= file.centerFrequencyHz - file.sampleRateHz / 2);
    assert.ok(file.view.f1 <= file.centerFrequencyHz + file.sampleRateHz / 2);
  });
  await check('I05 axis pan changes only its physical axis', async () => {
    const records = activeFile(await snapshot()).annotations;
    await dragPlot([.35, .23], [.65, .48]);
    const box = await page.locator('#specCanvas').boundingBox();
    let previous = activeFile(await snapshot()).view;
    await page.mouse.move(box.x + box.width * .5, box.y + box.height - 14);
    await page.mouse.down();
    await page.mouse.move(box.x + box.width * .5 + 50, box.y + box.height - 14, { steps: 4 });
    await page.mouse.up();
    await settle();
    let current = activeFile(await snapshot()).view;
    assert.notEqual(current.t0, previous.t0);
    assert.ok(Math.abs(current.f0 - previous.f0) <= 4 * Number.EPSILON * Math.abs(previous.f0));
    assert.ok(Math.abs(current.f1 - previous.f1) <= 4 * Number.EPSILON * Math.abs(previous.f1));
    previous = current;
    await page.mouse.move(box.x + 40, box.y + box.height * .4);
    await page.mouse.down();
    await page.mouse.move(box.x + 40, box.y + box.height * .4 + 40, { steps: 4 });
    await page.mouse.up();
    await settle();
    current = activeFile(await snapshot()).view;
    assert.notEqual(current.f0, previous.f0);
    assert.ok(Math.abs(current.t0 - previous.t0) <= 1e-12);
    assert.ok(Math.abs(current.t1 - previous.t1) <= 1e-12);
    assert.deepEqual(activeFile(await snapshot()).annotations, records);
  });
  await check('I07 overlap choice and Ctrl selection', async () => {
    const point = await markPoint({ startSample: '100000', endSample: '100001', frequencyLowerHz: 2492300000, frequencyUpperHz: 2492300001 }, 0, 0);
    await page.mouse.click(point.x, point.y);
    await settle();
    assert.ok(await page.locator('#overlapMenu button').count() >= 2);
    await page.locator('#overlapMenu button').first().click();
    await settle();
    assert.equal((await snapshot()).selectedIds.length, 1);
    await page.locator('#annotationRows [data-annotation="algo-coarse"]').click({ modifiers: ['Control'] });
    await settle();
    assert.equal((await snapshot()).selectedIds.length, 2);
    const ids = (await snapshot()).derived.visibleIds;
    await selectAnnotation(ids[1]);
    await page.locator(`#annotationRows [data-annotation="${ids.at(-1)}"]`).click({ modifiers: ['Shift'] });
    await settle();
    assert.deepEqual(new Set((await snapshot()).selectedIds), new Set(ids.slice(1)));
    assert.equal(await page.locator('#projectTree .tree-mark.selected').count(), ids.length - 1);
  });
  await check('I08 manual command, eight handles, undo and redo', async () => {
    await page.locator('#manualTool').click();
    const count = activeFile(await snapshot()).annotations.length;
    await dragPlot([.2, .18], [.55, .43]);
    let state = await snapshot();
    assert.equal(activeFile(state).annotations.length, count + 1);
    const id = state.activeAnnotationId;
    const origin = activeFile(state).annotations.find(a => a.id === id).origin;
    await page.locator('#manualTool').click();
    for (const [u, v] of [[0, 0], [.5, 0], [1, 0], [0, .5], [1, .5], [0, 1], [.5, 1], [1, 1]]) {
      state = await snapshot();
      const annotation = activeFile(state).annotations.find(a => a.id === id);
      const point = await markPoint(annotation.segments[0].range, u, v);
      const revision = annotation.revisionNumber;
      await page.mouse.move(point.x, point.y);
      await page.mouse.down();
      await page.mouse.move(point.x + 9, point.y + 9, { steps: 3 });
      await page.mouse.up();
      await settle();
      assert.equal(activeFile(await snapshot()).annotations.find(a => a.id === id).revisionNumber, revision + 1);
    }
    const edited = activeFile(await snapshot()).annotations.find(a => a.id === id);
    assert.deepEqual(edited.origin, origin);
    const range = edited.segments[0].range;
    await page.locator('#undoButton').click();
    await settle();
    assert.notDeepEqual(activeFile(await snapshot()).annotations.find(a => a.id === id).segments[0].range, range);
    await page.locator('#redoButton').click();
    await settle();
    assert.deepEqual(activeFile(await snapshot()).annotations.find(a => a.id === id).segments[0].range, range);
  });
  await check('I09 imported label requires revision and preserves origin', async () => {
    await selectAnnotation('label-1');
    assert.equal(await page.locator('#annotationDetail [data-geometry="t0"]').isDisabled(), true);
    const before = activeFile(await snapshot()).annotations.find(a => a.id === 'label-1');
    await page.locator('#annotationDetail [data-action="start-revision"]').click();
    await settle();
    await editStart('label-1');
    const after = activeFile(await snapshot()).annotations.find(a => a.id === 'label-1');
    assert.equal(after.sourceKind, 'ImportedLabel');
    assert.deepEqual(after.origin, before.origin);
    assert.notEqual(after.revisionId, before.revisionId);
  });
  await check('I09 review belongs to revision and geometry returns to Pending', async () => {
    await selectAnnotation('label-1');
    await page.locator('#batchAccept').click();
    await settle();
    let annotation = activeFile(await snapshot()).annotations.find(a => a.id === 'label-1');
    assert.equal(annotation.reviewStatus, 'Accepted');
    assert.equal(annotation.reviewHistory.at(-1).revisionId, annotation.revisionId);
    const historyLength = annotation.reviewHistory.length;
    await page.locator('#batchAccept').click();
    assert.equal(activeFile(await snapshot()).annotations.find(a => a.id === 'label-1').reviewHistory.length, historyLength);
    await page.locator('#annotationDetail [data-action="start-revision"]').click();
    await editStart('label-1');
    annotation = activeFile(await snapshot()).annotations.find(a => a.id === 'label-1');
    assert.equal(annotation.reviewStatus, 'Pending');
    assert.notEqual(annotation.reviewHistory[historyLength - 1].revisionId, annotation.revisionId);
  });
  await check('I17 file/project isolation for pending task and drafts', async () => {
    await scene('S5');
    const state = await snapshot();
    const second = state.files.find(f => f.id !== state.activeFileId);
    const annotations = second.annotations;
    await page.locator(`#projectTree [data-file="${second.id}"]`).click();
    await page.locator('[data-tab="tasks"]').click();
    const runId = state.runs.at(-1).id;
    for (let i = 0; i < 3; i++) { await page.locator(`[data-run-advance="${runId}"]`).click(); await settle(); }
    assert.equal((await snapshot()).activeFileId, second.id);
    assert.equal((await snapshot()).runs.find(r => r.id === runId).state, 'refine');
    assert.ok((await snapshot()).files.find(f => f.id === state.activeFileId).annotations.length > activeFile(state).annotations.length);
    assert.deepEqual(activeFile(await snapshot()).annotations, annotations);
    await page.screenshot({ path: path.join(output, 'screenshots', 'background-task-other-file.png') });
    await page.locator('[data-menu="fileMenu"]').click();
    await page.locator('#fileMenu [data-action="new-project"]').click();
    await settle();
    assert.equal((await snapshot()).runs.length, 0);
    assert.equal((await snapshot()).geometryDraft, null);
  });
  await check('I19 same-object segment split and merge', async () => {
    await selectAnnotation('manual-1');
    const before = activeFile(await snapshot()).annotations.find(a => a.id === 'manual-1');
    await page.locator('[data-edit-segments="split"]').click();
    await settle();
    assert.equal(activeFile(await snapshot()).annotations.find(a => a.id === 'manual-1').segments.length, before.segments.length + 1);
    await page.locator('[data-edit-segments="merge"]').click();
    await settle();
    const after = activeFile(await snapshot()).annotations.find(a => a.id === 'manual-1');
    assert.equal(after.segments.length, before.segments.length);
    assert.equal(after.sourceKind, 'Manual');
    assert.deepEqual(after.origin, before.origin);
    await page.locator('[data-edit-segments="add"]').click();
    await settle();
    assert.equal(activeFile(await snapshot()).annotations.find(a => a.id === 'manual-1').segments.length, before.segments.length + 1);
    await page.locator('[data-edit-segments="delete"]').click();
    await settle();
    assert.equal(activeFile(await snapshot()).annotations.find(a => a.id === 'manual-1').segments.length, before.segments.length);
  });
  await check('I20 both label previews and idempotent import', async () => {
    const count = activeFile(await snapshot()).annotations.length;
    await clickAction('import-labels');
    await clickAction('preview-import');
    await page.locator('#applyImport').click();
    await settle();
    assert.equal(activeFile(await snapshot()).annotations.length, count);
    await page.locator('#projectTree [data-file="file-b"]').click();
    const generatorCount = activeFile(await snapshot()).annotations.length;
    await clickAction('import-labels');
    await page.selectOption('#importType', 'generator');
    await clickAction('preview-import');
    assert.equal(await page.locator('#applyImport').isDisabled(), false);
    await page.locator('#applyImport').click();
    await settle();
    const once = activeFile(await snapshot()).annotations.length;
    assert.ok(once > generatorCount);
    await clickAction('import-labels');
    await page.selectOption('#importType', 'generator');
    await clickAction('preview-import');
    await page.locator('#applyImport').click();
    await settle();
    assert.equal(activeFile(await snapshot()).annotations.length, once);
  });
  await check('I20 actual writer TXT keeps FC axis and multi-burst quantization', async () => {
    await page.locator('#projectTree [data-file="file-b"]').click();
    assert.match(await page.locator('#fileDetails').innerText(), /双通道 PCM16 WAV/);
    await clickAction('import-labels');
    await page.selectOption('#importType', 'generator');
    await page.locator('#labelFiles').setInputFiles(path.join(root, 'tests/fixtures/wideband/generator-60m-10ms.txt'));
    await clickAction('preview-user-import');
    assert.equal(await page.locator('#applyImport').isDisabled(), false);
    assert.match(await page.locator('#importDiagnostic').innerText(), /10\s*(µs|μs|us)/);
    await page.locator('#applyImport').click();
    await settle();
    const file = activeFile(await snapshot());
    assert.equal(file.sampleRateHz, 60000000);
    const imported = file.annotations.filter(a => a.sourceKind === 'ImportedLabel');
    assert.equal(imported.length, 12);
    const first = imported.find(a => a.classLabel === 'BPSK');
    assert.ok(first);
    assert.equal(first.segments.length, 2);
    assert.equal(first.segments[0].range.startSample, '180000');
    assert.equal(first.segments[0].range.endSample, '300000');
    assert.equal(first.segments[0].range.frequencyLowerHz, file.centerFrequencyHz - file.sampleRateHz / 2 + 801339 - 229890 / 2);
    await selectAnnotation(first.id);
    assert.match(await page.locator('#annotationDetail').innerText(), /生成器 TXT 标签/);
    assert.match(await page.locator('#annotationDetail').innerText(), /10 µs 量化/);
    assert.doesNotMatch(await page.locator('#annotationDetail').innerText(), /TorchSig 样本标签/);
  });
  await check('I20 TorchSig JSON pairs independent of order and rejects class conflict', async () => {
    const meta = fs.readFileSync(path.join(root, 'tests/fixtures/wideband/torchsig-0000000.sigmf-meta'));
    const json = JSON.parse(fs.readFileSync(path.join(root, 'tests/fixtures/wideband/torchsig-0000000.json'), 'utf8'));
    await clickAction('import-labels');
    json.events.reverse();
    await page.locator('#labelFiles').setInputFiles([
      { name: '0000000.sigmf-meta', mimeType: 'application/json', buffer: meta },
      { name: '0000000.json', mimeType: 'application/json', buffer: Buffer.from(JSON.stringify(json)) }
    ]);
    await clickAction('preview-user-import');
    assert.equal(await page.locator('#applyImport').isDisabled(), false);
    json.events[0].modulation = 'WRONG_CLASS';
    await page.locator('#labelFiles').setInputFiles([
      { name: '0000000.sigmf-meta', mimeType: 'application/json', buffer: meta },
      { name: '0000000.json', mimeType: 'application/json', buffer: Buffer.from(JSON.stringify(json)) }
    ]);
    await clickAction('preview-user-import');
    assert.equal(await page.locator('#applyImport').isDisabled(), true);
    assert.match(await page.locator('#importDiagnostic').innerText(), /拒绝|冲突/);
  });
  await check('I21 conflict rejects import and Partial keeps full labels', async () => {
    const before = activeFile(await snapshot()).annotations;
    await clickAction('import-labels');
    await page.selectOption('#importSituation', 'conflict');
    await clickAction('preview-import');
    assert.equal(await page.locator('#applyImport').isDisabled(), true);
    assert.deepEqual(activeFile(await snapshot()).annotations, before);
    await page.screenshot({ path: path.join(output, 'screenshots', 'import-conflict.png') });
    await clickAction('close-modal');
    await scene('S2Partial');
    const file = activeFile(await snapshot());
    assert.ok(file.annotations.some(a => a.segments.some(s => Number(s.range.endSample) > file.availableSamples)));
  });
  await check('I22 channel stores old revision after source edit', async () => {
    await selectAnnotation('manual-1');
    await page.locator('#channelButton').click();
    await settle();
    await page.locator('#channelConfirmed').check();
    await page.locator('#channelRate').fill('0.01');
    await clickAction('create-channel');
    assert.equal(activeFile(await snapshot()).channels.length, 0);
    assert.match(await page.locator('#channelError').innerText(), /采样率/);
    await page.locator('#channelRate').fill('2');
    await clickAction('create-channel');
    const channel = activeFile(await snapshot()).channels.at(-1);
    assert.ok(channel && channel.demoOnly);
    await editStart('manual-1');
    const file = activeFile(await snapshot());
    assert.deepEqual(file.channels.at(-1).range, channel.range);
    assert.equal(file.channels.at(-1).revisionId, channel.revisionId);
    assert.notEqual(file.annotations.find(a => a.id === channel.annotationId).revisionId, channel.revisionId);
    assert.equal(file.annotations.find(a => a.id === channel.annotationId).reviewStatus, 'Pending');
    assert.match(await page.locator('#channelList').innerText(), /来源已修订，保留创建快照/);
    await page.screenshot({ path: path.join(output, 'screenshots', 'channel-source-changed.png') });
  });
  await check('I23 save/reopen and demo-only JSON download', async () => {
    const before = activeFile(await snapshot()).annotations;
    await clickAction('save');
    await scene('S0');
    await page.locator('[data-menu="fileMenu"]').click();
    await page.locator('#fileMenu [data-action="open-project"]').click();
    await settle();
    assert.deepEqual(activeFile(await snapshot()).annotations, before);
    const downloadPromise = page.waitForEvent('download');
    await clickAction('export');
    const download = await downloadPromise;
    const target = path.join(output, 'demo-export.json');
    await download.saveAs(target);
    assert.equal(JSON.parse(fs.readFileSync(target, 'utf8')).demoOnly, true);
    await scene('SaveError');
    const preserved = activeFile(await snapshot()).annotations;
    await clickAction('save');
    assert.deepEqual(activeFile(await snapshot()).annotations, preserved);
  });
  await check('I23 malformed saved state is rejected atomically', async () => {
    const before = await snapshot();
    await page.locator('[data-menu="fileMenu"]').click();
    await page.locator('#fileMenu [data-action="help"]').count().then(async count => {
      if (count) await page.locator('#fileMenu [data-action="help"]').click();
      else await clickAction('help');
    });
    await clickAction('open-saved-import');
    for (const corrupt of [s => { delete s.layout; }, s => { s.params.nfft = 'invalid'; }, s => { s.files[0].annotations[0].segments[0].range.endSample = 'NaN'; }]) {
      const saved = await page.evaluate(() => window.signalStudioPrototype.dispatch('exportSnapshot'));
      saved.project.name = 'CORRUPTED_REPLACED';
      corrupt(saved);
      await page.locator('#savedFile').setInputFiles({ name: 'corrupt.json', mimeType: 'application/json', buffer: Buffer.from(JSON.stringify(saved)) });
      await clickAction('read-saved');
      assert.match(await page.locator('#savedError').innerText(), /拒绝/);
      const after = await snapshot();
      assert.deepEqual(after.project, before.project);
      assert.deepEqual(after.files, before.files);
      assert.deepEqual(after.runs, before.runs);
      assert.deepEqual(after.layout, before.layout);
    }
    await clickAction('close-modal');
  });
  await check('I24 maximize and layout restore preserve geometry', async () => {
    const records = activeFile(await snapshot()).annotations;
    await page.locator('#spectralPanel [data-action="maximize"]').click();
    await settle();
    assert.equal(await page.locator('#spectralPanel').evaluate(el => el.classList.contains('maximized')), true);
    await page.locator('#spectralPanel [data-action="maximize"]').click();
    await settle();
    await page.locator('[data-menu="viewMenu"]').click();
    await page.locator('#viewMenu [data-action="restore-layout"]').click();
    await settle();
    assert.deepEqual(activeFile(await snapshot()).annotations, records);
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false);
  });
  await check('I24 three dividers and folding keep usable layout', async () => {
    const records = activeFile(await snapshot()).annotations;
    for (const [kind, dx, dy] of [['left', 35, 0], ['right', -35, 0], ['bottom', 0, -35]]) {
      const before = (await snapshot()).layout[kind];
      await dragElement(`[data-resize="${kind}"]`, dx, dy);
      assert.notEqual((await snapshot()).layout[kind], before);
    }
    for (const key of ['tree', 'inspector', 'bottom']) {
      await clickAction(`toggle-${key}`);
      assert.equal(await page.locator('#app').evaluate((el, name) => el.classList.contains(`collapsed-${name}`), key), true);
      await clickAction(`toggle-${key}`);
    }
    assert.deepEqual(activeFile(await snapshot()).annotations, records);
    const plot = await page.locator('#specCanvas').boundingBox();
    assert.ok(plot.width > 600 && plot.height > 180);
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false);
    await page.screenshot({ path: path.join(output, 'screenshots', 'adjusted-layout.png') });
  });
  await scene('S8');
  await page.screenshot({ path: path.join(output, 'screenshots', 'review-evidence.png') });
  await scene('S9');
  await page.screenshot({ path: path.join(output, 'screenshots', 'channel-source.png') });
  assert.equal(errors.length, 0, `Browser errors: ${errors.join('; ')}`);
} finally {
  const report = { date: new Date().toLocaleDateString('en-CA', { timeZone: 'Asia/Shanghai' }), recordedAtUtc: new Date().toISOString(), url, browser: browser.version(), viewport: { width: 2560, height: 1440, deviceScaleFactor: 1 },
    evidence: 'Browser prototype with synthetic data; not Qt, hardware DPI, DSP or model validation', checks, errors,
    passed: checks.filter(c => c.status === 'passed').length, failed: checks.filter(c => c.status === 'failed').length };
  fs.writeFileSync(path.join(output, 'browser-checks.json'), JSON.stringify(report, null, 2) + '\n');
  await browser.close();
  console.log(JSON.stringify({ passed: report.passed, failed: report.failed, errors: report.errors }));
  if (report.failed || report.errors.length || !report.checks.length) process.exitCode = 1;
}
