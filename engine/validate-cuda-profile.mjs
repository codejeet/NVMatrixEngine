import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {pathToFileURL} from 'node:url';

export function summary(values) {
  assert.ok(values.length, 'No latency samples');
  for (const x of values) assert.ok(Number.isFinite(x) && x >= 0, 'Invalid timing');
  const a = [...values].sort((x, y) => x - y);
  const q = p => a[Math.floor(p * (a.length - 1))];
  return {median: q(.5), p95: q(.95), p99: q(.99), max: a.at(-1),
    missed60Hz: a.filter(x => x > 1000 / 60).length, count: a.length};
}

export const cpuColumns = ['frameIndex', 'renderBegin', 'fluidBegin', 'cudaPrepareBegin', 'cudaPrepared',
  'interopBegin', 'interopContextReady', 'prefixClosed', 'prefixSubmitted', 'dxReleased', 'cudaWaitQueued',
  'cudaStartQueued', 'cudaWorkQueued', 'cudaEndQueued', 'cudaReleaseQueued', 'dxWaitQueued', 'interopResumed',
  'fluidRecorded', 'surfaceRecorded', 'photonsRecorded', 'cameraRecorded', 'compositeRecorded', 'rrBegin',
  'rrRecorded', 'uiRecorded', 'frameClosed', 'frameSubmitted', 'presented', 'frameFenceSignaled',
  'frameEventQueued', 'frameComplete', 'collected'];

export function validateCpuSubmission(r, required = false) {
  const p = r.latency.cpuSubmission;
  if (p === undefined) {
    assert.ok(!required, 'Missing CPU submission timeline');
    return null; // Historic artifacts do not contain evidence for these intervals.
  }
  assert.equal(p.version, 1);
  assert.equal(p.clock, 'steady_clock');
  assert.equal(p.units, 'milliseconds since renderBegin; CPU wall time, not GPU time');
  assert.deepEqual(p.columns, cpuColumns);
  assert.equal(p.rows.length, r.latency.rows.length);
  assert.ok(['cuda', 'dx12'].includes(r.fluid.backend), 'Unknown CPU trace backend');
  const cuda = r.fluid.backend === 'cuda';
  const index = name => cpuColumns.indexOf(name);
  for (const [i, row] of p.rows.entries()) {
    assert.equal(row.length, cpuColumns.length);
    assert.equal(row[0], r.latency.rows[i][0], 'CPU/GPU frame identities disagree');
    assert.equal(row[1], 0, 'CPU trace origin must be zero');
    let previous = 0;
    for (let j = 1; j < row.length; ++j) {
      const cudaStage = j >= index('cudaPrepareBegin') && j <= index('interopResumed');
      if (cudaStage && !cuda) {
        assert.equal(row[j], null, 'DX12 trace invented CUDA work');
        continue;
      }
      if (j === index('frameEventQueued') && row[j] === null) continue; // Already complete.
      assert.ok(typeof row[j] === 'number' && Number.isFinite(row[j]) && row[j] >= previous,
        `Missing or nonmonotonic CPU stage ${cpuColumns[j]} at frame ${i}`);
      previous = row[j];
    }
    const whole = r.latency.rows[i][1], renderer = r.latency.rows[i][7];
    const tolerance = .002 + whole * 1e-5; // Main timing columns retain their historic JSON precision.
    assert.ok(previous <= whole + tolerance && previous >= renderer - tolerance,
      'CPU trace does not cover the measured renderer interval');
  }
  const ranges = [
    ['renderSetup', 'renderBegin', 'fluidBegin'],
    ['fluidRecordTotal', 'fluidBegin', 'fluidRecorded'],
    ['surfaceRecord', 'fluidRecorded', 'surfaceRecorded'],
    ['photonsRecord', 'surfaceRecorded', 'photonsRecorded'],
    ['cameraRecord', 'photonsRecorded', 'cameraRecorded'],
    ['compositeRecord', 'cameraRecorded', 'compositeRecorded'],
    ['rrRecord', 'rrBegin', 'rrRecorded'],
    ['uiRecord', 'rrRecorded', 'uiRecorded'],
    ['frameClose', 'uiRecorded', 'frameClosed'],
    ['frameSubmit', 'frameClosed', 'frameSubmitted'],
    ['present', 'frameSubmitted', 'presented'],
    ['frameSignal', 'presented', 'frameFenceSignaled'],
    ['frameWait', 'frameFenceSignaled', 'frameComplete'],
    ['collection', 'frameComplete', 'collected'],
  ];
  if (cuda) ranges.push(
    ['fluidSetup', 'fluidBegin', 'cudaPrepareBegin'],
    ['cudaPreparation', 'cudaPrepareBegin', 'cudaPrepared'],
    ['interopContext', 'interopBegin', 'interopContextReady'],
    ['prefixClose', 'interopContextReady', 'prefixClosed'],
    ['prefixSubmit', 'prefixClosed', 'prefixSubmitted'],
    ['dxRelease', 'prefixSubmitted', 'dxReleased'],
    ['cudaWait', 'dxReleased', 'cudaWaitQueued'],
    ['cudaStart', 'cudaWaitQueued', 'cudaStartQueued'],
    ['cudaEnqueue', 'cudaStartQueued', 'cudaWorkQueued'],
    ['cudaEnd', 'cudaWorkQueued', 'cudaEndQueued'],
    ['cudaRelease', 'cudaEndQueued', 'cudaReleaseQueued'],
    ['dxWait', 'cudaReleaseQueued', 'dxWaitQueued'],
    ['interopResume', 'dxWaitQueued', 'interopResumed'],
    ['postCudaRecord', 'interopResumed', 'frameClosed'],
    ['prefixToSuffixSubmission', 'prefixSubmitted', 'frameSubmitted']);
  const rows = p.rows.map(row => ({frame: row[0], milliseconds: Object.fromEntries(ranges.map(
    ([name, begin, end]) => [name, row[index(end)] - row[index(begin)]]))}));
  const phases = Object.fromEntries([['cold', 0, 1], ['warmup', 0, 32], ['steady', 32, 300]].map(
    ([phase, lo, hi]) => [phase, Object.fromEntries(ranges.map(([name]) => [name,
      summary(rows.slice(lo, hi).map(row => row.milliseconds[name]))]))]));
  const worstFrames = r.latency.rows.slice(32).toSorted((a, b) => b[1] - a[1]).slice(0, 10).map(row => ({
    frame: row[0], wholeFrame: row[1], cudaWork: row[13], cudaHandoff: row[14],
    cpu: rows[row[0]].milliseconds}));
  return {phases, worstFrames};
}

export function validateProfile(r, {requireCpu = false} = {}) {
  assert.equal(r.frames, 300);
  assert.equal(r.sampleCount, 268);
  assert.equal(r.dlssEvaluations, 300);
  assert.equal(r.frameGeneration.enabled, false, 'Generated frames are not raw frames');
  assert.equal(r.fluid.validated, false, 'Do not profile validation snapshots');
  assert.equal(r.fluid.steps, 600);
  assert.equal(r.fluid.droppedSeconds, 0);
  const p = r.latency;
  assert.equal(p.version, 1);
  assert.equal(p.completeFrames, 300);
  assert.equal(p.rows.length, 300);
  assert.deepEqual(p.columns, ['frameIndex', 'wholeFrame', 'photons', 'atlasEma', 'camera',
    'composite', 'dlssRR', 'rendererFrame', 'fluidSimulation', 'fluidReconstruction',
    'fluidBlas', 'whitewaterSimulation', 'whitewaterBlas', 'cudaWork', 'cudaHandoff',
    'cudaSpan', 'cudaCpuEnqueue', 'cudaPreparationTotal', 'simulatedSeconds',
    'droppedSeconds', 'localUsageBytes', 'localBudgetBytes', 'memoryQueryMs']);
  for (const [i, row] of p.rows.entries()) {
    assert.equal(row.length, p.columns.length);
    for (const v of row) assert.ok(Number.isFinite(v) && v >= 0);
    assert.equal(row[0], i);
    assert.ok(row[1] >= row[7], 'Whole frame must include the render loop');
    assert.ok(Math.abs(row[18] - (i + 1) / 60) < 1e-5, 'Unequal simulated time');
    assert.equal(row[19], 0);
    assert.ok(row[20] > 0 && row[21] > 0, 'Missing memory observation');
    if (i) assert.ok(row[17] >= p.rows[i - 1][17], 'Cumulative graph preparation regressed');
  }
  const peak = Math.max(...p.rows.map(row => row[20]));
  // JSON display precision may round the independently stored byte peak.
  assert.ok(Math.abs(p.sampledPeakLocalUsageBytes - peak) <= peak * 1e-5);
  assert.ok(p.startupMs >= 0 && p.firstFrameCompletedMs > p.startupMs);
  const phases = {};
  for (const [name, lo, hi] of [['cold', 0, 1], ['warmup', 0, 32], ['steady', 32, 300],
    ['inletWindow', 90, 120], ['impactWindow', 180, 240]]) {
    phases[name] = summary(p.rows.slice(lo, hi).map(row => row[1]));
  }
  const cpuSubmission = validateCpuSubmission(r, requireCpu);
  return {backend: r.fluid.backend, phases, sampledPeakMiB: peak / 2 ** 20,
    startupMs: p.startupMs, firstFrameCompletedMs: p.firstFrameCompletedMs,
    cudaCpuEnqueue: summary(p.rows.slice(32).map(row => row[16])), cpuSubmission};
}

if (process.argv[1] && pathToFileURL(process.argv[1]).href === import.meta.url) {
  const requireCpu = process.argv.includes('--require-cpu');
  const files = process.argv.slice(2).filter(x => x !== '--require-cpu');
  assert.ok(files.length, 'Pass one or more latency capture JSON files');
  for (const file of files) {
    const result = validateProfile(JSON.parse(readFileSync(file, 'utf8').replace(/^\uFEFF/, '')), {requireCpu});
    console.log(JSON.stringify({file, ...result}));
  }
}
