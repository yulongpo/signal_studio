// Browser checks only. These do not launch Qt, consume IQ, or execute a model/DDC.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const require = createRequire(import.meta.url);
const { chromium } = require(process.env.SS_PLAYWRIGHT_MODULE || 'playwright');
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const output = path.resolve(
  process.env.SS_U0_OUTPUT || path.join(root, 'out/tasks/wideband-detection/U0')
);
const url =
  process.env.SS_U0_URL ||
  'http://127.0.0.1:8785/docs/prototype/annotation-interactions-u0/index.html';
fs.mkdirSync(path.join(output, 'assets'), { recursive: true });
fs.mkdirSync(path.join(output, 'diagnostics'), { recursive: true });
const browser = await chromium.launch({ channel: 'msedge', headless: true });
const context = await browser.newContext({
  viewport: { width: 2560, height: 1440 },
  deviceScaleFactor: 1,
});
const page = await context.newPage();
page.setDefaultTimeout(6000);
const checks = [];
const pageErrors = [];
page.on('pageerror', error => pageErrors.push(error.message));
const settle = () => page.waitForTimeout(80);
const snapshot = () =>
  page.evaluate(() => window.annotationPrototype.snapshot());
const file = state => state.files.find(item => item.id === state.activeFileId);
const annotation = (state, id) =>
  file(state).annotations.find(item => item.id === id);
const active = state => annotation(state, file(state).selection.activeId);
const row = id => page.locator(`#annotationRows tr[data-annotation="${id}"]`);
const tree = id => page.locator(`#treePanel [data-annotation="${id}"]`);
const action = async (name, within = page) => {
  await within.locator(`[data-action="${name}"]:visible`).first().click();
  await settle();
};
async function scene(value = 'standard') {
  if (
    !(await page
      .locator('#modalBackdrop')
      .evaluate(element => element.classList.contains('hidden')))
  )
    await page.keyboard.press('Escape');
  await page.selectOption('#scenario', value);
  await action('reset');
}
async function select(id, surface = 'table', modifiers = []) {
  const element = surface === 'tree' ? tree(id) : row(id).locator('td').nth(3);
  await element.click({ modifiers });
  await settle();
}
async function editCell(id, field, value, key = 'Enter') {
  await row(id).locator(`[data-field="${field}"]`).dblclick();
  const editor = page.locator('[data-cell-editor]');
  await editor.fill(String(value));
  await editor.press(key);
  await settle();
}
async function quality(value) {
  await page.locator('.record-actions .quality-menu > summary').click();
  await page.locator(`.record-actions [data-quality="${value}"]`).click();
  await settle();
}
async function image(name) {
  await page.waitForFunction(
    () => !document.getElementById('toast').classList.contains('show'),
    null,
    { timeout: 4000 }
  );
  // Wait for the existing 100 ms CSS fade before saving the review image.
  await page.waitForTimeout(160);
  await page.screenshot({ path: path.join(output, 'assets', `${name}.png`) });
}
async function point(id, segmentId = null, u = 0.5, v = 0.5) {
  const geometry = await page.evaluate(() =>
    window.annotationPrototype.geometry()
  );
  const marker = geometry.markers.find(
    item =>
      item.id === id && (segmentId === null || item.segmentId === segmentId)
  );
  assert.ok(marker, `Missing plot geometry for ${id}`);
  return {
    x: geometry.viewport.x + marker.x + marker.width * u,
    y: geometry.viewport.y + marker.y + marker.height * v,
  };
}
async function drag(
  id,
  segmentId,
  dx,
  dy,
  { cancel = false, handle = null } = {}
) {
  const position = handle
    ? await page
        .locator(
          `#marksSvg [data-annotation="${id}"][data-segment-id="${segmentId}"] [data-handle="${handle}"]`
        )
        .boundingBox()
    : null;
  const start = position
    ? {
        x: position.x + position.width / 2,
        y: position.y + position.height / 2,
      }
    : await point(id, segmentId);
  await page.mouse.move(start.x, start.y);
  await page.mouse.down();
  await page.mouse.move(start.x + dx, start.y + dy, { steps: 5 });
  if (cancel) await page.keyboard.press('Escape');
  await page.mouse.up();
  await settle();
}
async function check(name, fn) {
  try {
    await scene();
    await fn();
    checks.push({ name, status: 'passed' });
    console.log(`PASS ${name}`);
  } catch (error) {
    checks.push({ name, status: 'failed', error: error.message });
    console.log(`FAIL ${name}: ${error.message.slice(0, 240)}`);
    await image(`failure-${checks.length}`);
    await page.keyboard.press('Escape');
  }
}

try {
  await page.goto(url);
  await page.waitForFunction(() => Boolean(window.annotationPrototype));
  await settle();
  await image('01-ready');
  await check('U0-01 layout and explicit demo boundary', async () => {
    assert.equal(
      await page.evaluate(
        () => document.documentElement.scrollWidth > innerWidth
      ),
      false
    );
    assert.match(
      await page.locator('body').innerText(),
      /不读取 IQ.*不执行真实 DDC/
    );
    assert.equal(file(await snapshot()).annotations.length, 8);
    assert.equal(pageErrors.length, 0);
  });
  for (const id of ['label-1', 'manual-1', 'algo-ignored']) {
    await check(`U0 selection table to tree and plot: ${id}`, async () => {
      const before = cloneView(await snapshot());
      await select(id);
      assert.equal(file(await snapshot()).selection.activeId, id);
      assert.equal(await tree(id).getAttribute('aria-selected'), 'true');
      assert.ok(
        await page
          .locator(`#marksSvg [data-annotation="${id}"].selected`)
          .count()
      );
      assert.deepEqual(cloneView(await snapshot()), before);
    });
    await check(`U0 selection tree to table and plot: ${id}`, async () => {
      await select(id, 'tree');
      assert.equal(await row(id).getAttribute('aria-selected'), 'true');
      assert.match(
        await page.locator('#annotationInspector').innerText(),
        new RegExp(id)
      );
      const base = await page
        .locator(`#marksSvg [data-annotation="${id}"] .source-rect`)
        .first()
        .getAttribute('stroke');
      assert.ok(['#65d2bd', '#edc56d', '#51c4e9'].includes(base));
    });
    await check(
      `U0 direct table name edit with immutable origin: ${id}`,
      async () => {
        const before = annotation(await snapshot(), id);
        await editCell(id, 'name', `${before.name} revised`);
        const after = annotation(await snapshot(), id);
        assert.equal(after.name, `${before.name} revised`);
        assert.equal(after.id, before.id);
        assert.equal(after.sourceKind, before.sourceKind);
        assert.deepEqual(after.origin, before.origin);
        assert.equal(after.reviewStatus, before.reviewStatus);
        assert.equal(after.revisionNumber, 1);
        await action('undo');
        assert.equal(annotation(await snapshot(), id).name, before.name);
      }
    );
  }
  await check(
    'U0-11 actual clicked row is current during Ctrl and Shift selection',
    async () => {
      await select('label-1');
      await select('manual-1', 'table', ['Control']);
      let selected = file(await snapshot()).selection;
      assert.equal(selected.activeId, 'manual-1');
      assert.equal(selected.ids.length, 2);
      await select('label-3', 'table', ['Shift']);
      selected = file(await snapshot()).selection;
      assert.equal(selected.activeId, 'label-3');
      assert.ok(selected.ids.includes('label-3'));
    }
  );
  await check(
    'U0-12 hover links three surfaces without selection mutation',
    async () => {
      const before = file(await snapshot()).selection.ids;
      await tree('manual-1').hover();
      assert.ok(
        await row('manual-1').evaluate(element =>
          element.classList.contains('hovered')
        )
      );
      assert.ok(
        await page
          .locator('#marksSvg [data-annotation="manual-1"].hovered')
          .count()
      );
      assert.deepEqual(file(await snapshot()).selection.ids, before);
      await page.mouse.move(500, 10);
    }
  );
  await check(
    'U0-13 multiple segments expose only current edit handles',
    async () => {
      await select('manual-1');
      assert.equal(
        await page
          .locator('#marksSvg [data-annotation="manual-1"].selected')
          .count(),
        2
      );
      assert.equal(
        await page
          .locator('#marksSvg [data-annotation="manual-1"] .handle')
          .count(),
        8
      );
      await page.locator('[data-segment="manual-1-s2"]').click();
      assert.equal(file(await snapshot()).selection.segmentId, 'manual-1-s2');
      assert.match(await row('manual-1').innerText(), /第 2\/2 段/);
      assert.equal(
        await row('manual-1').locator('[data-field="startMs"]').innerText(),
        '9.0000'
      );
      await image('02-multisegment');
    }
  );
  for (const id of ['label-4', 'manual-1', 'algo-ignored']) {
    await check(
      `U0 direct graphic drag is one revision and one undo: ${id}`,
      async () => {
        await select(id);
        const before = annotation(await snapshot(), id);
        const segmentId = before.activeSegmentId;
        await drag(id, segmentId, 12, -5);
        const after = annotation(await snapshot(), id);
        assert.notDeepEqual(
          after.segments.find(item => item.id === segmentId).range,
          before.segments.find(item => item.id === segmentId).range
        );
        assert.deepEqual(after.origin, before.origin);
        assert.equal(after.revisionNumber, 1);
        assert.equal((await snapshot()).undoCount, 1);
        assert.equal(after.reviewStatus, 'Pending');
        await action('undo');
        assert.deepEqual(
          annotation(await snapshot(), id).segments,
          before.segments
        );
      }
    );
  }
  await check('U0-17 graphic Esc cancels preview without command', async () => {
    await select('manual-1');
    const before = annotation(await snapshot(), 'manual-1');
    await drag('manual-1', 'manual-1-s1', 25, -8, { cancel: true });
    assert.deepEqual(annotation(await snapshot(), 'manual-1'), before);
    assert.equal((await snapshot()).undoCount, 0);
  });
  await check(
    'U0-18 unknown time allows frequency editing without inventing segments',
    async () => {
      const before = annotation(await snapshot(), 'algo-observation');
      assert.equal(
        await row('algo-observation').locator('[data-field="startMs"]').count(),
        0
      );
      await editCell('algo-observation', 'centerMHz', 2500.2);
      const after = annotation(await snapshot(), 'algo-observation');
      assert.equal(after.segments.length, 0);
      assert.equal(
        after.observation.startSample,
        before.observation.startSample
      );
      assert.equal(after.observation.endSample, before.observation.endSample);
      assert.notEqual(
        after.observation.frequencyLowerHz,
        before.observation.frequencyLowerHz
      );
      assert.deepEqual(after.origin, before.origin);
    }
  );
  await check('U0-19 complete range edit validates atomically', async () => {
    await select('manual-1');
    const before = annotation(await snapshot(), 'manual-1');
    await action('range');
    await page.fill('#rangeStart', '7');
    await page.fill('#rangeEnd', '5');
    await action('confirm-modal');
    assert.match(
      await page.locator('#dialogErrors').innerText(),
      /起点|小于|范围/
    );
    assert.deepEqual(annotation(await snapshot(), 'manual-1'), before);
    await page.fill('#rangeStart', '4.2');
    await page.fill('#rangeEnd', '6.2');
    await action('confirm-modal');
    assert.equal((await snapshot()).undoCount, 1);
    assert.deepEqual(
      annotation(await snapshot(), 'manual-1').segments[1],
      before.segments[1]
    );
    await image('03-direct-revision');
  });
  await check(
    'U0-20 table input commits only Enter and Esc preserves value',
    async () => {
      await row('label-1').locator('[data-field="name"]').dblclick();
      await page.locator('[data-cell-editor]').fill('draft-only');
      assert.notEqual(
        annotation(await snapshot(), 'label-1').name,
        'draft-only'
      );
      await page.locator('[data-cell-editor]').press('Escape');
      assert.notEqual(
        annotation(await snapshot(), 'label-1').name,
        'draft-only'
      );
      assert.equal((await snapshot()).undoCount, 0);
      await editCell('label-1', 'name', 'committed-name');
      assert.equal(
        annotation(await snapshot(), 'label-1').name,
        'committed-name'
      );
    }
  );
  await check(
    'U0-21 Tab continues editing and Delete inside input does not delete object',
    async () => {
      await row('label-1').locator('[data-field="name"]').dblclick();
      const editor = page.locator('[data-cell-editor]');
      await editor.fill('Name one');
      await editor.press('Tab');
      assert.equal(annotation(await snapshot(), 'label-1').name, 'Name one');
      assert.equal((await snapshot()).draft.field, 'startMs');
      await page.locator('[data-cell-editor]').press('Control+A');
      await page.locator('[data-cell-editor]').press('Delete');
      assert.equal(annotation(await snapshot(), 'label-1').deleted, false);
      assert.ok(
        await page
          .locator('#modalBackdrop')
          .evaluate(element => element.classList.contains('hidden'))
      );
      await page.keyboard.press('Escape');
    }
  );
  await check(
    'U0-22 background publish preserves draft and selection',
    async () => {
      await row('label-1').locator('[data-field="name"]').dblclick();
      await page
        .locator('[data-cell-editor]')
        .fill('uncommitted background draft');
      const before = await snapshot();
      await action('publish');
      const after = await snapshot();
      assert.equal(after.draft.text, 'uncommitted background draft');
      assert.deepEqual(file(after).selection.ids, file(before).selection.ids);
      assert.equal(
        annotation(after, 'label-1').name,
        annotation(before, 'label-1').name
      );
      assert.equal(
        file(after).annotations.length,
        file(before).annotations.length + 1
      );
      await image('04-background-draft');
      await page.keyboard.press('Escape');
    }
  );
  await check(
    'U0-23 context switch cancels draft instead of committing',
    async () => {
      await row('label-1').locator('[data-field="name"]').dblclick();
      await page.locator('[data-cell-editor]').fill('discard on switch');
      await page.locator('[data-file="file-b"]').click();
      await page.locator('[data-file="file-a"]').click();
      assert.notEqual(
        annotation(await snapshot(), 'label-1').name,
        'discard on switch'
      );
      assert.equal((await snapshot()).undoCount, 0);
    }
  );
  await check(
    'U0-24 >200 cross-page selection and full matching select-all',
    async () => {
      await scene('large');
      await action('select-all');
      assert.equal(file(await snapshot()).selection.ids.length, 268);
      assert.equal(await page.locator('#annotationRows tr').count(), 200);
      await action('next-page');
      assert.equal(file(await snapshot()).selection.ids.length, 268);
      assert.equal(await page.locator('#annotationRows tr').count(), 68);
      await image('05-cross-page');
    }
  );
  await check(
    'U0-25 plot selection loads tree item beyond 100 and jumps to correct table page',
    async () => {
      await scene('large');
      // Select from a visible table page, then graph-select after page navigation.
      await action('next-page');
      await select('bulk-260');
      assert.ok(await tree('bulk-260').count());
      await action('previous-page');
      const position = await point('bulk-260');
      await page.mouse.click(position.x, position.y);
      await settle();
      assert.equal(file(await snapshot()).selection.activeId, 'bulk-260');
      assert.equal((await snapshot()).page, 1);
      assert.equal(await row('bulk-260').getAttribute('aria-selected'), 'true');
    }
  );
  await check(
    'U0-26 filters and layers prune hidden selections before batch deletion',
    async () => {
      await scene('large');
      await action('select-all');
      await page.selectOption('#sourceFilter', 'Algorithm');
      let current = await snapshot();
      assert.ok(file(current).selection.ids.length > 80);
      assert.ok(
        file(current).selection.ids.every(
          id => annotation(current, id).sourceKind === 'Algorithm'
        )
      );
      await page.locator('[data-layer="Algorithm"]').uncheck();
      current = await snapshot();
      assert.equal(file(current).selection.ids.length, 0);
      assert.equal(
        await page.locator('#annotationRows [data-annotation]').count(),
        0
      );
    }
  );
  await check(
    'U0-27 sorting preserves actual current object and page',
    async () => {
      await scene('large');
      await action('next-page');
      await select('bulk-260');
      await page.locator('th[data-sort="name"]').click();
      assert.equal(file(await snapshot()).selection.activeId, 'bulk-260');
      assert.ok(await row('bulk-260').count());
      assert.ok(await tree('bulk-260').count());
    }
  );
  await check(
    'U0-28 optional quality never gates channel and does not alter current snapshot',
    async () => {
      await select('algo-located');
      await quality('Rejected');
      assert.match(await page.locator('#channelList').innerText(), /范围一致/);
      await action('channel');
      assert.equal(await page.locator('#confirmModal').isEnabled(), true);
      await action('confirm-modal');
      assert.equal(
        annotation(await snapshot(), 'algo-located').reviewStatus,
        'Rejected'
      );
      assert.equal(file(await snapshot()).channels.length, 2);
    }
  );
  await check(
    'U0-29 name and note preserve channel relation, geometry changes relation',
    async () => {
      await select('algo-located');
      await quality('Accepted');
      await editCell('algo-located', 'name', 'renamed algorithm');
      await action('note');
      await page.fill('#textEdit', 'review note');
      await action('confirm-modal');
      assert.equal(active(await snapshot()).reviewStatus, 'Accepted');
      assert.match(await page.locator('#channelList').innerText(), /范围一致/);
      await editCell('algo-located', 'endMs', '7.2');
      assert.equal(active(await snapshot()).reviewStatus, 'Pending');
      assert.match(
        await page.locator('#channelList').innerText(),
        /来源范围有差异/
      );
      await action('undo');
      assert.match(await page.locator('#channelList').innerText(), /范围一致/);
    }
  );
  await check(
    'U0-30 unknown-time Pending result creates UserRange without algorithm segments',
    async () => {
      const before = active(await snapshot());
      await action('channel');
      assert.match(
        await page.locator('#channelScope').innerText(),
        /UserRange.*人工提取范围/s
      );
      assert.equal(await page.inputValue('#channelStart'), '0');
      assert.equal(await page.inputValue('#channelEnd'), '20');
      await image('06-user-range');
      await action('confirm-modal');
      const current = await snapshot();
      const channel = file(current).channels.at(-1);
      assert.equal(channel.scopeOrigin, 'UserRange');
      assert.equal(channel.segmentId, null);
      assert.deepEqual(active(current).segments, before.segments);
      assert.equal(active(current).reviewStatus, before.reviewStatus);
      assert.equal(channel.range.endSample, 400000);
      assert.equal(
        channel.sourceSnapshot.referenceRange.endSample,
        before.observation.endSample
      );
    }
  );
  await check(
    'U0-31 located time trimming binds only active segment',
    async () => {
      await select('algo-located');
      await page.locator('[data-segment="algo-located-s2"]').click();
      await action('channel');
      await page.fill('#channelStart', '10');
      await page.fill('#channelEnd', '16');
      await action('confirm-modal');
      const channel = file(await snapshot()).channels.at(-1);
      assert.equal(channel.scopeOrigin, 'LocatedSegment');
      assert.equal(channel.segmentId, 'algo-located-s2');
      assert.equal(channel.range.startSample, 200000);
      assert.equal(channel.range.endSample, 320000);
    }
  );
  await check(
    'U0-32 Partial and empty intersection require explicit user range',
    async () => {
      await scene('partial');
      await action('channel');
      assert.equal(await page.inputValue('#channelEnd'), '12');
      await page.keyboard.press('Escape');
      await scene('outside');
      await action('channel');
      assert.equal(await page.inputValue('#channelStart'), '12');
      assert.equal(await page.inputValue('#channelEnd'), '12');
      assert.equal(await page.locator('#confirmModal').isEnabled(), false);
      await image('07-empty-intersection');
      await action('use-prefix');
      assert.equal(await page.inputValue('#channelStart'), '0');
      assert.equal(await page.inputValue('#channelEnd'), '12');
      assert.equal(await page.locator('#confirmModal').isEnabled(), true);
    }
  );
  await check(
    'U0-33 physical source changes and unreadable source show specific diagnostic',
    async () => {
      await scene('changed-source');
      await action('channel');
      assert.equal(await page.locator('#confirmModal').isEnabled(), false);
      assert.match(
        await page.locator('#channelErrors').innerText(),
        /身份|来源|不一致/
      );
      await page.keyboard.press('Escape');
      await scene('missing-source');
      await action('channel');
      assert.equal(await page.locator('#confirmModal').isEnabled(), false);
      assert.match(
        await page.locator('#channelErrors').innerText(),
        /读取|可读|来源|物理源/
      );
    }
  );
  await check(
    'U0-34 invalid DSP parameters identify field and do not create channel',
    async () => {
      await action('channel');
      await page.fill('#channelRate', '0.1');
      assert.equal(await page.locator('#confirmModal').isEnabled(), false);
      assert.match(
        await page.locator('#channelErrors').innerText(),
        /采样率|outputSampleRate/
      );
      assert.equal(file(await snapshot()).channels.length, 1);
      await page.fill('#channelRate', '4');
      await page.fill('#channelCenter', '2510');
      assert.equal(await page.locator('#confirmModal').isEnabled(), false);
      assert.match(
        await page.locator('#channelErrors').innerText(),
        /频|保护带/
      );
    }
  );
  await check(
    'U0-35 batch delete preview and one undo preserve channels',
    async () => {
      await select('algo-located');
      await select('manual-1', 'tree', ['Control']);
      await action('delete');
      assert.equal(await page.locator('#deleteObjectCount').innerText(), '2');
      assert.equal(await page.locator('#deleteSegmentCount').innerText(), '4');
      assert.equal(await page.locator('#deleteChannelCount').innerText(), '1');
      await image('08-delete-preview');
      await action('confirm-modal');
      let current = await snapshot();
      assert.equal(annotation(current, 'algo-located').deleted, true);
      assert.equal(file(current).channels.length, 1);
      assert.match(
        await page.locator('#treeChannels').innerText(),
        /来源已删除/
      );
      await action('undo');
      current = await snapshot();
      assert.equal(annotation(current, 'algo-located').deleted, false);
      assert.equal(annotation(current, 'manual-1').deleted, false);
      assert.match(await page.locator('#treeChannels').innerText(), /范围一致/);
    }
  );
  await check(
    'U0-36 delete current segment and last-segment warning are distinct',
    async () => {
      await select('manual-1');
      await action('delete-segment');
      assert.equal(await page.locator('#deleteObjectCount').innerText(), '0');
      await action('confirm-modal');
      assert.equal(active(await snapshot()).segments.length, 1);
      await action('delete-segment');
      assert.match(
        await page.locator('#modalTitle').innerText(),
        /最后一段.*删除标记/
      );
      await action('confirm-modal');
      assert.equal(annotation(await snapshot(), 'manual-1').deleted, true);
    }
  );
  await check(
    'U0-37 multi-selection channel entry explicitly chooses one object',
    async () => {
      await select('label-1');
      await select('algo-observation', 'tree', ['Control']);
      await action('channel');
      assert.match(await page.locator('#modalTitle').innerText(), /选择一个/);
      await page.locator('[data-channel-source="algo-observation"]').click();
      await settle();
      assert.match(
        await page.locator('#channelScope').innerText(),
        /UserRange/
      );
      assert.equal(file(await snapshot()).selection.ids.length, 1);
    }
  );
  await check(
    'U0-38 zoom and mode preserve selection and physical source',
    async () => {
      await select('manual-1');
      const before = file(await snapshot()).selection;
      await action('zoom');
      await action('waterfall');
      const after = file(await snapshot()).selection;
      assert.deepEqual(after.ids, before.ids);
      assert.equal(after.segmentId, before.segmentId);
      assert.equal((await snapshot()).mode, 'waterfall');
      await image('09-waterfall');
    }
  );
  await check(
    'U0-39 save and reopen preserves UserRange, Missing channel and quality',
    async () => {
      await action('channel');
      await action('confirm-modal');
      await select('algo-located');
      await action('delete');
      await action('confirm-modal');
      await action('save');
      const before = await snapshot();
      await action('reset');
      await action('reopen');
      const after = await snapshot();
      assert.equal(after.version, 6);
      assert.equal(after.demoOnly, true);
      assert.deepEqual(file(after).channels, file(before).channels);
      assert.deepEqual(file(after).annotations, file(before).annotations);
      assert.match(
        await page.locator('#treeChannels').innerText(),
        /来源已删除/
      );
      await image('10-reopen');
    }
  );
  for (const id of ['label-4', 'manual-1', 'algo-ignored']) {
    await check(
      `U0 graph click selects source with no implicit zoom: ${id}`,
      async () => {
        await action('clear-selection');
        const before = cloneView(await snapshot());
        const position = await point(id, null, 0.85, 0.5);
        await page.mouse.click(position.x, position.y);
        await settle();
        assert.equal(file(await snapshot()).selection.activeId, id);
        assert.equal(await tree(id).getAttribute('aria-selected'), 'true');
        assert.equal(await row(id).getAttribute('aria-selected'), 'true');
        assert.deepEqual(cloneView(await snapshot()), before);
      }
    );
  }
  await check(
    'U0-43 undo preserves background publication and current view',
    async () => {
      await editCell('manual-1', 'name', 'name before publication');
      await action('publish');
      await action('zoom');
      const before = await snapshot();
      const publication = file(before).annotations.find(item =>
        item.id.startsWith('published-')
      );
      await action('undo');
      const after = await snapshot();
      assert.deepEqual(annotation(after, publication.id), publication);
      assert.deepEqual(cloneView(after), cloneView(before));
      assert.notEqual(
        annotation(after, 'manual-1').name,
        'name before publication'
      );
      await action('redo');
      assert.deepEqual(
        annotation(await snapshot(), publication.id),
        publication
      );
    }
  );
  await check(
    'U0-44 unknown time graphic editing retains observation times',
    async () => {
      const before = active(await snapshot());
      await drag('algo-observation', null, 30, -12);
      const after = active(await snapshot());
      assert.equal(after.segments.length, 0);
      assert.equal(
        after.observation.startSample,
        before.observation.startSample
      );
      assert.equal(after.observation.endSample, before.observation.endSample);
      assert.notEqual(
        after.observation.frequencyLowerHz,
        before.observation.frequencyLowerHz
      );
    }
  );
  await check('U0-45 handle resize only affects current segment', async () => {
    await select('manual-1');
    await page.locator('[data-segment="manual-1-s2"]').click();
    const before = active(await snapshot());
    await drag('manual-1', 'manual-1-s2', 15, 0, { handle: 'e' });
    const after = active(await snapshot());
    assert.deepEqual(after.segments[0], before.segments[0]);
    assert.notEqual(
      after.segments[1].range.endSample,
      before.segments[1].range.endSample
    );
    assert.equal((await snapshot()).undoCount, 1);
  });
  await check(
    'U0-46 F2 and application blur preserve uncommitted draft',
    async () => {
      await select('manual-1');
      await row('manual-1').locator('[data-field="name"]').focus();
      await page.keyboard.press('F2');
      await page.locator('[data-cell-editor]').fill('keep during blur');
      const before = active(await snapshot()).name;
      await page.evaluate(() => window.dispatchEvent(new Event('blur')));
      assert.equal(active(await snapshot()).name, before);
      assert.equal((await snapshot()).draft.text, 'keep during blur');
      await page.evaluate(() => window.dispatchEvent(new Event('focus')));
      await page.locator('[data-cell-editor]').press('Escape');
      assert.equal(active(await snapshot()).name, before);
    }
  );
  await check('U0-47 old background box zoom and axis pan remain', async () => {
    await select('manual-1');
    const before = file(await snapshot()).selection.ids;
    let geometry = await page.evaluate(() =>
      window.annotationPrototype.geometry()
    );
    const x = geometry.viewport.x + geometry.left + geometry.width * 0.05;
    const y = geometry.viewport.y + geometry.top + geometry.height * 0.04;
    await page.mouse.move(x, y);
    await page.mouse.down();
    await page.mouse.move(
      x + geometry.width * 0.23,
      y + geometry.height * 0.18,
      { steps: 5 }
    );
    await page.mouse.up();
    await settle();
    const boxed = cloneView(await snapshot());
    assert.ok(boxed.endMs - boxed.startMs < 10);
    assert.deepEqual(file(await snapshot()).selection.ids, before);
    geometry = await page.evaluate(() => window.annotationPrototype.geometry());
    const axisX = geometry.viewport.x + geometry.left + geometry.width * 0.5;
    const axisY = geometry.viewport.y + geometry.top + geometry.height + 15;
    await page.mouse.move(axisX, axisY);
    await page.mouse.down();
    await page.mouse.move(axisX + 80, axisY, { steps: 5 });
    await page.mouse.up();
    await settle();
    const panned = cloneView(await snapshot());
    assert.notEqual(panned.startMs, boxed.startMs);
    assert.equal(panned.lowerMHz, boxed.lowerMHz);
    assert.equal(panned.upperMHz, boxed.upperMHz);
    assert.deepEqual(file(await snapshot()).selection.ids, before);
  });
  await check(
    'U0-48 axis wheel only zooms its physical coordinate',
    async () => {
      await action('zoom');
      const before = cloneView(await snapshot());
      const geometry = await page.evaluate(() =>
        window.annotationPrototype.geometry()
      );
      await page.mouse.move(
        geometry.viewport.x + geometry.left + geometry.width * 0.5,
        geometry.viewport.y + geometry.top + geometry.height + 15
      );
      await page.mouse.wheel(0, -120);
      await settle();
      const after = cloneView(await snapshot());
      assert.ok(after.endMs - after.startMs < before.endMs - before.startMs);
      assert.equal(after.lowerMHz, before.lowerMHz);
      assert.equal(after.upperMHz, before.upperMHz);
    }
  );
  await check(
    'U0-49 parameter wheel preserves value and score applies only on submit',
    async () => {
      const input = page.locator('#scoreFilter');
      await input.fill('0.9');
      assert.equal((await snapshot()).filters.score, 0);
      await input.hover();
      await page.mouse.wheel(0, 120);
      await settle();
      assert.equal(await input.inputValue(), '0.9');
      assert.equal((await snapshot()).filters.score, 0);
      await input.press('Enter');
      assert.equal((await snapshot()).filters.score, 0.9);
      assert.equal(await row('label-1').count(), 0);
      assert.equal(await row('algo-located').count(), 1);
    }
  );
  await check(
    'U0-50 split and merge retain ID and evidence with one-step undo',
    async () => {
      await select('manual-1');
      const before = active(await snapshot());
      await action('split-segment');
      assert.equal(active(await snapshot()).segments.length, 3);
      assert.deepEqual(active(await snapshot()).origin, before.origin);
      await action('undo');
      assert.deepEqual(active(await snapshot()).segments, before.segments);
      await action('merge-segment');
      assert.equal(active(await snapshot()).segments.length, 1);
      assert.equal(active(await snapshot()).id, before.id);
      await action('undo');
      assert.deepEqual(active(await snapshot()).segments, before.segments);
    }
  );
  await check(
    'U0-51 same context actions from table, tree and plot',
    async () => {
      for (const surface of ['table', 'tree', 'plot']) {
        await select('algo-observation');
        if (surface === 'plot') {
          const position = await point('algo-observation');
          await page.mouse.click(position.x, position.y, { button: 'right' });
        } else
          await (
            surface === 'tree'
              ? tree('algo-observation')
              : row('algo-observation')
          ).click({ button: 'right' });
        assert.match(
          await page.locator('#contextMenu').innerText(),
          /编辑当前范围.*创建窄带通道.*删除所选标记/s
        );
        await page.locator('#contextMenu [data-action="channel"]').click();
        await settle();
        assert.match(
          await page.locator('#channelScope').innerText(),
          /UserRange/
        );
        await page.keyboard.press('Escape');
      }
    }
  );
  await check(
    'U0-52 gesture interruption rolls back drag without a revision',
    async () => {
      await select('manual-1');
      const before = active(await snapshot());
      const position = await point('manual-1', 'manual-1-s1');
      await page.mouse.move(position.x, position.y);
      await page.mouse.down();
      await page.mouse.move(position.x + 25, position.y - 8, { steps: 5 });
      await page.evaluate(() => window.dispatchEvent(new Event('blur')));
      await page.mouse.up();
      await page.evaluate(() => window.dispatchEvent(new Event('focus')));
      assert.deepEqual(active(await snapshot()), before);
      assert.equal((await snapshot()).undoCount, 0);
    }
  );
  await check(
    'U0-53 active segment sorting matches displayed values and keeps unknown times last',
    async () => {
      await select('algo-located');
      await page.locator('[data-segment="algo-located-s2"]').click();
      await page.locator('th[data-sort="start"]').click();
      const values = await page
        .locator('#annotationRows tr')
        .evaluateAll(rows => rows.map(row => row.cells[4].textContent.trim()));
      const numbers = values.filter(value => value !== '—').map(Number);
      assert.deepEqual(
        numbers,
        [...numbers].sort((a, b) => a - b)
      );
      assert.equal(values.at(-1), '—');
    }
  );
  await check(
    'U0-54 save reload publish preserves unique IDs and reopen validity',
    async () => {
      await action('publish');
      await action('save');
      await page.reload();
      await page.waitForFunction(() => Boolean(window.annotationPrototype));
      await action('reopen');
      await action('publish');
      const ids = file(await snapshot()).annotations.map(item => item.id);
      assert.equal(new Set(ids).size, ids.length);
      await action('save');
      await action('reset');
      await action('reopen');
      assert.equal(file(await snapshot()).annotations.length, ids.length);
    }
  );
  await check('U0-55 Cancel and Close keep their Enter semantics', async () => {
    await select('label-1');
    await action('delete');
    await page.locator('#modalActions [data-action="close-modal"]').focus();
    await page.keyboard.press('Enter');
    await settle();
    assert.equal(annotation(await snapshot(), 'label-1').deleted, false);
    assert.equal((await snapshot()).undoCount, 0);
    await action('delete');
    await page.locator('.modal-heading [data-action="close-modal"]').focus();
    await page.keyboard.press('Enter');
    await settle();
    assert.equal(annotation(await snapshot(), 'label-1').deleted, false);
    assert.equal((await snapshot()).undoCount, 0);
  });
  await check(
    'U0-56 application click commits once and performs requested next action',
    async () => {
      for (const surface of ['table', 'tree', 'inspector']) {
        await scene();
        await row('label-1').locator('[data-field="name"]').dblclick();
        await page.locator('[data-cell-editor]').fill('name via click');
        if (surface === 'table')
          await row('manual-1').locator('td').nth(3).click();
        else if (surface === 'tree') await tree('manual-1').click();
        else
          await page
            .locator('#annotationInspector [data-action="range"]')
            .click();
        await settle();
        assert.equal(
          annotation(await snapshot(), 'label-1').name,
          'name via click'
        );
        assert.equal((await snapshot()).undoCount, 1);
        if (surface === 'inspector') {
          assert.match(
            await page.locator('#modalTitle').innerText(),
            /完整范围/
          );
          assert.equal(
            await page
              .locator('#modalBackdrop')
              .evaluate(element => element.classList.contains('hidden')),
            false
          );
        } else
          assert.equal(file(await snapshot()).selection.activeId, 'manual-1');
      }
    }
  );
  await check(
    'U0-57 unchanged numeric fields are exact no-ops and do not stale a channel',
    async () => {
      await select('label-1');
      await action('channel');
      await action('confirm-modal');
      const before = await snapshot();
      for (const field of ['startMs', 'endMs', 'centerMHz', 'bandwidthKHz']) {
        await row('label-1').locator(`[data-field="${field}"]`).dblclick();
        await page.locator('[data-cell-editor]').press('Enter');
        await settle();
      }
      await action('range');
      await action('confirm-modal');
      const after = await snapshot();
      assert.deepEqual(
        annotation(after, 'label-1'),
        annotation(before, 'label-1')
      );
      assert.equal(after.undoCount, before.undoCount);
      assert.match(await page.locator('#channelList').innerText(), /范围一致/);
      const range = active(after).segments[0].range;
      await editCell(
        'label-1',
        'startMs',
        (range.startSample / file(after).sampleRateHz) * 1000 + 0.01
      );
      const changed = active(await snapshot()).segments[0].range;
      assert.equal(changed.frequencyLowerHz, range.frequencyLowerHz);
      assert.equal(changed.frequencyUpperHz, range.frequencyUpperHz);
      assert.equal(changed.endSample, range.endSample);
    }
  );
  await check(
    'U0-58 active row is revealed below the sticky header',
    async () => {
      await scene('large');
      await action('next-page');
      await select('bulk-260');
      await page.locator('th[data-sort="name"]').click();
      await scene();
      const visible = await row('algo-observation').evaluate(element => {
        const header = document
          .querySelector('#annotationTable thead')
          .getBoundingClientRect();
        const viewport = document
          .getElementById('tableScroll')
          .getBoundingClientRect();
        const bounds = element.getBoundingClientRect();
        return (
          bounds.top >= header.bottom - 1 &&
          bounds.bottom <= viewport.bottom + 1
        );
      });
      assert.equal(visible, true);
    }
  );
  await check(
    'U0-40 no console exceptions across all interactions',
    async () => {
      assert.deepEqual(pageErrors, []);
    }
  );
} finally {
  const report = {
    schema: 'signalstudio-annotation-u0-browser-checks-v1',
    demoOnly: true,
    url,
    browser: await browser.version(),
    viewport: { width: 2560, height: 1440, deviceScaleFactor: 1 },
    scope:
      'Browser interaction only; no native Qt, real IQ, model, DDC or monitor acceptance.',
    passed: checks.filter(item => item.status === 'passed').length,
    failed: checks.filter(item => item.status === 'failed').length,
    pageErrors,
    checks,
  };
  fs.writeFileSync(
    path.join(output, 'diagnostics/browser-checks.json'),
    JSON.stringify(report, null, 2)
  );
  await browser.close();
  console.log(
    `Browser checks: ${report.passed}/${checks.length} passed; ${report.failed} failed.`
  );
  if (report.failed || pageErrors.length) process.exitCode = 1;
}

function cloneView(state) {
  return structuredClone(file(state).view);
}
