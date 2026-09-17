import test from 'node:test';
import assert from 'node:assert/strict';
import {summary, validateProfile, cpuColumns} from './validate-cuda-profile.mjs';

function capture() {
  const columns = ['frameIndex', 'wholeFrame', 'photons', 'atlasEma', 'camera',
    'composite', 'dlssRR', 'rendererFrame', 'fluidSimulation', 'fluidReconstruction',
    'fluidBlas', 'whitewaterSimulation', 'whitewaterBlas', 'cudaWork', 'cudaHandoff',
    'cudaSpan', 'cudaCpuEnqueue', 'cudaPreparationTotal', 'simulatedSeconds',
    'droppedSeconds', 'localUsageBytes', 'localBudgetBytes', 'memoryQueryMs'];
  return {frames: 300, sampleCount: 268, dlssEvaluations: 300, frameGeneration: {enabled: false},
    fluid: {backend: 'cuda', validated: false, steps: 600, droppedSeconds: 0},
    latency: {version: 1, completeFrames: 300, startupMs: 2000, firstFrameCompletedMs: 2100,
      sampledPeakLocalUsageBytes: 2 ** 30, columns,
      rows: Array.from({length: 300}, (_, i) => [i, i ? 15 : 100, 1, .1, 4, .1, 3, i ? 14 : 99,
        3, 1, .05, .1, .01, 2.8, .1, 2.9, .2, 4, (i + 1) / 60, 0, 2 ** 30, 2 ** 34, .01])}};
}
test('latency quantiles retain maximum and missed deadlines separately', () => {
  const a = [...Array(99).fill(10), 90];
  assert.deepEqual(summary(a), {median: 10, p95: 10, p99: 10, max: 90, missed60Hz: 1, count: 100});
  assert.throws(() => summary([]));
  assert.throws(() => summary([NaN]));
});
test('cold start stays visible without contaminating steady samples', () => {
  const r = validateProfile(capture());
  assert.equal(r.phases.cold.max, 100);
  assert.equal(r.phases.steady.median, 15);
  assert.equal(r.phases.steady.count, 268);
  assert.equal(r.sampledPeakMiB, 1024);
});
test('profile rejects incomplete samples and generated-frame substitutions', () => {
  for (const edit of [r => r.latency.rows.pop(), r => r.frameGeneration.enabled = true,
    r => r.fluid.validated = true, r => r.latency.rows[20][0] = 19]) {
    const r = capture(); edit(r); assert.throws(() => validateProfile(r));
  }
});
test('profile rejects dropped physical time, omitted CPU work and missing memory', () => {
  for (const edit of [r => r.latency.rows[299][18] = 4.9, r => r.latency.rows[10][19] = .1,
    r => r.latency.rows[20][1] = 1, r => r.latency.rows[10][20] = 0,
    r => r.latency.rows[10][17] = 3, r => r.latency.rows[10][13] = Infinity]) {
    const r = capture(); edit(r); assert.throws(() => validateProfile(r));
  }
});
function cpuCapture(backend = 'cuda') {
  const r = capture();
  r.fluid.backend = backend;
  r.latency.cpuSubmission = {version: 1, clock: 'steady_clock',
    units: 'milliseconds since renderBegin; CPU wall time, not GPU time', columns: [...cpuColumns],
    rows: r.latency.rows.map(row => cpuColumns.map((name, j) => {
      if (!j) return row[0];
      if (backend === 'dx12' && j >= 3 && j <= 16) return null;
      return row[7] * (j - 1) / (cpuColumns.length - 2);
    }))};
  return r;
}
test('CPU submission stages preserve frame identity, missing DX12 CUDA work, and cold start', () => {
  for (const backend of ['cuda', 'dx12']) {
    const r = cpuCapture(backend);
    const p = validateProfile(r, {requireCpu: true}).cpuSubmission;
    assert.equal(p.phases.steady.rrRecord.count, 268);
    assert.ok(p.phases.cold.rrRecord.max > p.phases.steady.rrRecord.max);
    assert.equal(p.worstFrames.length, 10);
    assert.equal(p.phases.steady.cudaWait !== undefined, backend === 'cuda');
    r.latency.cpuSubmission.rows[50][cpuColumns.indexOf('frameEventQueued')] = null;
    assert.doesNotThrow(() => validateProfile(r, {requireCpu: true}));
  }
});
test('CPU timeline validation rejects omissions, schema drift, reordered or impossible timestamps', () => {
  for (const edit of [r => delete r.latency.cpuSubmission, r => r.latency.cpuSubmission.rows.pop(),
    r => r.latency.cpuSubmission.columns.reverse(), r => r.latency.cpuSubmission.rows[7][0] = 6,
    r => r.latency.cpuSubmission.rows[7][1] = .1, r => r.latency.cpuSubmission.rows[7][10] = null,
    r => r.latency.cpuSubmission.rows[7][10] = '4', r => r.latency.cpuSubmission.rows[7][10] = NaN,
    r => r.latency.cpuSubmission.rows[7][10] = Infinity, r => r.latency.cpuSubmission.rows[7][10] = -1,
    r => r.latency.cpuSubmission.rows[7][10] = 0, r => r.latency.cpuSubmission.rows[7].push(20),
    r => r.latency.cpuSubmission.rows[7][31] = 30, r => r.latency.cpuSubmission.clock = 'gpu']) {
    const r = cpuCapture(); edit(r); assert.throws(() => validateProfile(r, {requireCpu: true}));
  }
  const dx = cpuCapture('dx12'); dx.latency.cpuSubmission.rows[7][10] = 0;
  assert.throws(() => validateProfile(dx, {requireCpu: true}));
});
test('historic reports remain analyzable without inventing CPU submission evidence', () => {
  assert.equal(validateProfile(capture()).cpuSubmission, null);
  assert.throws(() => validateProfile(capture(), {requireCpu: true}));
});
test('late CPU recording and GPU handoff stay separate observables', () => {
  const r = cpuCapture();
  const row = r.latency.cpuSubmission.rows[78];
  const rr = cpuColumns.indexOf('rrRecorded');
  for (let j = rr; j < row.length; ++j) row[j] += 100;
  r.latency.rows[78][1] += 100;
  r.latency.rows[78][7] += 100;
  // Handoff is deliberately unchanged: a CPU interval is not proof of GPU causality.
  const p = validateProfile(r, {requireCpu: true}).cpuSubmission;
  assert.equal(p.worstFrames[0].frame, 78);
  assert.ok(p.worstFrames[0].cpu.rrRecord > 100);
  assert.equal(p.worstFrames[0].cudaHandoff, .1);
});
