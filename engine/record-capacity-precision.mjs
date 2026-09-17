// Read-only evidence collector. Save the checked output with apply_patch.
import {readFileSync, readdirSync, statSync} from 'node:fs';
import {join} from 'node:path';
import {createHash} from 'node:crypto';
import {execFileSync} from 'node:child_process';
import assert from 'node:assert/strict';
import {readCapture} from './validate-captures.mjs';

const folder = process.argv[2];
const sourceBase = execFileSync('git', ['rev-parse','d19a3f9'], {encoding:'utf8'}).trim();
if (!folder || process.argv.length !== 3)
  throw Error('Usage: node engine/record-capacity-precision.mjs RUNTIME');
const hash = path => createHash('sha256').update(readFileSync(path)).digest('hex');
const exe = join(folder, 'NVMatrixFluidLab.exe');
const shaderNames = readdirSync(join(folder, 'shaders')).filter(n =>
  /^(Carrier.*-coupled|(?:Bulk|Cut|Implicit|Allocation).*-precise|MacApplyCapacityCut|Mg(?:Import|Export)CorrectionCut)\.dxil$/.test(n)).sort();
assert.ok(shaderNames.length >= 55);
const built = Math.max(statSync(exe).mtimeMs, ...shaderNames.map(n => statSync(join(folder, 'shaders', n)).mtimeMs));
const expected = {'initial-calm':1, 'initial-room':1, empty:4, 'gravity-start':1,
  short:8, calm:120, fall:90, room:120, 'room-no-tension':120, wake:180,
  controls:12, reset:180, adaptive:64, overlay:32};
const cases = Object.entries(expected).map(([name, frames]) => {
  const prefix = join(folder, `bulk-coupled-${name}-time`);
  for (const ext of ['.json', '.inputs', '.ppm'])
    assert.ok(statSync(prefix + ext).mtimeMs >= built, `Stale ${name}${ext}`);
  const {report:r, summary:optical} = readCapture(prefix);
  assert.equal(r.frames, frames);
  const f=r.fluid, b=f.bulk, c=f.cutCells, m=f.adaptiveMac, p=b.carrierProjection;
  const i=b.implicitTransport, s=f.bulkPressure, a=b.sourceAllocation;
  assert.ok(f.validated && b.validated && c.validated && m.validated && m.multigrid.validated && a.validated);
  assert.equal(b.inventoryBits, 64); assert.equal(c.coarseCapacityBits, 64); assert.equal(i.inventoryBits, 64);
  assert.equal(b.steps, f.steps); assert.equal(i.steps, f.steps); assert.equal(p.steps, f.steps);
  assert.equal(b.invalid+c.invalid+m.invalid+m.multigrid.exhaustedSolves+p.invalid+p.exhaustedSteps+p.isolatedRows+i.invalid+i.exhaustedSteps+s.invalid+a.invalid, 0);
  for (const v of [p,i,s]) assert.equal(v.auditedFrames+v.auditedIdleFrames, frames);
  assert.equal(p.auditedSubsteps, p.totalSubsteps);
  assert.ok(!p.steps || p.validated); assert.ok(!i.steps || i.validated);
  assert.ok(!s.projectionCalls || s.validated);
  assert.ok(p.mixedPressureCoupled && c.timeCenteredPressure && b.projectedFlux);
  assert.ok(p.auditedPhaseDeficit <= 1.001e-12 && p.auditedPhaseResidual <= 5.01e-7);
  assert.ok(p.originalPhysicalDivergence <= .000101 && p.proposedPhysicalDivergence <= 1.001e-6 && p.appliedPhysicalDivergence <= 1.01e-6);
  assert.ok(p.appliedVelocityError <= 2e-6 && m.multigrid.peakFinalDivergence <= .000101);
  assert.ok(b.capacityRestrictionMaxError <= 1e-14 && b.fluxRestrictionMaxError <= 1e-10);
  assert.ok(i.tolerance <= 2.01e-13 && i.peakResidual <= 2.01e-13 && i.peakExcessM3 <= 1e-11);
  assert.equal(c.bulkInClosedCellsM3, 0);
  assert.ok(b.relativeVolumeError <= .0002 && b.maxVolumeFraction <= 1.00001 && a.newExcessM3 <= 1e-7);
  assert.equal(r.photonCounters[5]+r.photonCounters[6]+r.fluidProbes.badRoots+r.fluidProbes.truncated, 0);
  return {name, frames, reportSha256:hash(prefix+'.json'), captureSha256:hash(prefix+'.inputs'),
    steps:f.steps, volumeM3:b.volumeM3, relativeVolumeError:b.relativeVolumeError,
    maxVolumeFraction:b.maxVolumeFraction, excessVolumeM3:b.excessVolumeM3,
    capacityRestrictionMaxError:b.capacityRestrictionMaxError, implicit:i, carrier:p,
    optical};
});
const compatibility = ['bulk-air-initial-calm-time', 'bulk-air-controls-time', 'bulk-implicit-short-time'].map(name => {
  const path=join(folder, name+'.json');
  assert.ok(statSync(path).mtimeMs >= statSync(exe).mtimeMs, `Stale ${name}`);
  const r=JSON.parse(readFileSync(path)), b=r.fluid.bulk;
  assert.equal(b.inventoryBits,32); assert.ok(b.validated && r.fluid.bulkPressure.validated);
  assert.equal(b.invalid+b.implicitTransport.invalid+b.implicitTransport.exhaustedSteps,0);
  if(name==='bulk-implicit-short-time') {
    assert.equal(b.excessVolumeM3,.00625802);
    assert.equal(b.maxVolumeFraction,1.76809); // preserve the known unbounded baseline, not an acceptance claim
  }
  return {name, frames:r.frames, reportSha256:hash(path), excessVolumeM3:b.excessVolumeM3};
});
const launches = ['launch-basename','launch-relative','launch-foreign-cwd','lab'].map(name => {
  const path=join(folder,name+'.json');
  assert.ok(statSync(path).mtimeMs >= statSync(exe).mtimeMs, `Stale launch ${name}`);
  const r=JSON.parse(readFileSync(path));
  assert.equal(r.dlssEvaluations,r.frames);
  if(name==='lab') assert.ok(r.frames>=15 && r.pacedPresentation && r.rrHistoryResets>=3);
  else assert.equal(r.frames,48);
  return {name, frames:r.frames, reportSha256:hash(path)};
});
const sourceNames = [...new Set([
  ...execFileSync('git', ['diff','--name-only',sourceBase,'--','engine'], {encoding:'utf8'}).trim().split('\n'),
  ...execFileSync('git', ['ls-files','--others','--exclude-standard','--','engine'], {encoding:'utf8'}).trim().split('\n')
])].filter(n => /\.(cpp|h|hlsl|hlsli|ps1|mjs)$/.test(n)).sort();
console.log(JSON.stringify({schema:1, generatedUtc:new Date().toISOString(),
  sourceBase,
  scope:'Opt-in coupled capacity precision and paused-boundary conservation; not flowing ownership or performance acceptance',
  readyForDefault:false, performanceClaim:false,
  exeSha256:hash(exe), hardwareFixtureExeSha256:hash(join(folder,'NVMatrixEngineCarrierGpuTest.exe')),
  sourceSha256:Object.fromEntries(sourceNames.map(n=>[n,hash(n)])),
  shaderSha256:Object.fromEntries(shaderNames.map(n=>[n,hash(join(folder,'shaders',n))])),
  debugLayerValidated:false, gbvValidated:false, compatibility, launches, cases}, null, 2));
