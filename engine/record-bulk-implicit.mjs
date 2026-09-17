// Read-only checkpoint collector. Persist the checked JSON with apply_patch.
import {readFileSync,readdirSync,statSync} from 'node:fs';
import {join} from 'node:path';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
import {readCapture} from './validate-captures.mjs';
const folder=process.argv[2];
const profiled=process.argv[3]==='--profiles';
if(!folder||(process.argv[3]&&!profiled))throw new Error('Usage: node engine/record-bulk-implicit.mjs RUNTIME [--profiles]');
const exe=join(folder,'NVMatrixFluidLab.exe'),shaders=join(folder,'shaders');
const names=readdirSync(shaders).filter(n=>n.endsWith('.dxil')).sort();
const built=Math.max(statSync(exe).mtimeMs,...names.map(n=>statSync(join(shaders,n)).mtimeMs));
function read(name){
  const prefix=join(folder,name);
  for(const ext of ['.json','.inputs'])assert.ok(statSync(prefix+ext).mtimeMs>=built,`Stale ${name}${ext}`);
  const r=JSON.parse(readFileSync(prefix+'.json'));
  return {r,optical:readCapture(prefix).summary};
}
function check(r,implicit){
  const f=r.fluid,b=f.bulk,c=f.cutCells,m=f.adaptiveMac,s=f.bulkPressure,i=b.implicitTransport,a=b.sourceAllocation;
  assert.ok(f.validated&&b.validated&&m.validated&&c.validated&&a.validated&&m.multigrid.validated);
  assert.ok(c.timeCenteredPressure&&m.cutPressure&&b.projectedFlux);
  assert.equal(b.steps,f.steps);assert.equal(!!i,implicit);
  assert.equal(b.invalid+c.invalid+m.invalid+a.invalid+m.multigrid.exhaustedSolves+s.invalid,0);
  assert.ok(b.relativeVolumeError<=.0002&&a.newExcessM3<=1e-7);
  assert.ok(b.fluxRestrictionMaxError<=1e-10&&b.capacityRestrictionMaxError<=1e-7);
  assert.ok(c.maxTimeAreaError<=1e-4&&m.multigrid.peakFinalDivergence<=.000101);
  assert.ok(!s.projectionCalls||s.validated);assert.equal(s.auditedFrames+s.auditedIdleFrames,r.frames);
  assert.equal(s.partialClosingSupport,implicit);
  assert.ok(s.velocityReferenceError<=2e-6&&s.weightReferenceError<=2e-6);
  assert.equal(r.photonCounters[5]+r.photonCounters[6]+r.fluidProbes.badRoots+r.fluidProbes.truncated,0);
  if(implicit){
    assert.equal(b.phaseLimiter,null);assert.ok(!i.steps||i.validated);
    assert.equal(i.steps,b.steps);assert.equal(i.invalid+i.exhaustedSteps,0);
    assert.equal(i.auditedFrames+i.auditedIdleFrames,r.frames);
    assert.ok(i.peakResidual<=2.01e-10&&i.matrixReferenceError<=2.02e-10);
    assert.equal(c.bulkInClosedCellsM3,0,'Closing-cell water was not evacuated');
  }else{
    const l=b.phaseLimiter;assert.ok(!l.steps||l.validated);
    assert.equal(l.steps,b.steps);assert.equal(l.invalid+l.exhaustedSteps,0);
    assert.ok(l.peakNewExcessM3<=1e-7);
  }
  return {steps:b.steps,volumeM3:b.volumeM3,massError:b.relativeVolumeError,excessM3:b.excessVolumeM3,
    closedM3:c.bulkInClosedCellsM3,maxFraction:b.maxVolumeFraction,
    particleMass:f.restMassUnits,particleDensity:f.postStepMaxRelativeDensity,
    surfaceVolume:r.fluidSurface.tetrahedralVolumeEstimate,
    pressurePeakDivergence:m.multigrid.peakFinalDivergence,
    closingPressureUpdates:s.partialClosingCellUpdates,
    closingWaterUpdates:i?.closingWaterUpdates??0,
    implicit:i??null};
}
const cases=Object.entries({'initial-calm':1,'initial-room':1,empty:4,short:8,calm:120,fall:90,
  room:120,'room-no-tension':120,wake:180,controls:12,reset:180,adaptive:64,overlay:32}).map(([name,frames])=>{
  const {r,optical}=read(`bulk-implicit-${name}-time`);assert.equal(r.frames,frames);
  const result=check(r,true);
  if(name==='reset')assert.ok(result.closingPressureUpdates>0&&result.closingWaterUpdates>0);
  if(name==='wake')assert.ok(result.closingWaterUpdates>0);
  return {name,frames,...result,optical};
});
const comparisons=['calm','room','room-no-tension','wake','adaptive'].map(name=>{
  const {r,optical}=read(`bulk-bounded-${name}-time-support`),before=check(r,false);
  const after=cases.find(c=>c.name===name);assert.equal(r.frames,after.frames);
  assert.equal(before.particleMass,after.particleMass);
  const densityDifference=after.particleDensity-before.particleDensity;
  const surfaceVolumeRelativeDifference=Math.abs(after.surfaceVolume/before.surfaceVolume-1);
  return {name,reference:{...before,optical},densityDifference,surfaceVolumeRelativeDifference,
    physicalPreservationPassed:densityDifference<=.01&&surfaceVolumeRelativeDifference<=.01,
    excessDifferenceM3:after.excessM3-before.excessM3};
});
// Do not turn positive/conservative transport into an upper-bound claim.
const failures=cases.filter(c=>c.excessM3>1e-7).map(c=>`${c.name}: ${c.excessM3} m3 above endpoint capacity`);
for(const c of comparisons)if(!c.physicalPreservationPassed)failures.push(`${c.name}: particle density or surface-volume preservation failed`);
const profiles=[];
if(profiled)for(let repeat=1;repeat<=3;repeat++)for(const mode of ['reference','implicit']){
  const {r}=read(`bulk-implicit-cost-${mode}-${repeat}`),f=r.fluid,b=f.bulk,i=b.implicitTransport,m=f.adaptiveMac,c=f.cutCells,s=f.bulkPressure;
  assert.equal(r.frames,300);assert.equal(r.sampleCount,268);assert.equal(f.steps,600);assert.equal(f.droppedSeconds,0);
  assert.ok(!r.frameGeneration.enabled&&!f.validated&&!m.auditedFrames&&!c.auditedFrames&&!b.validated);
  assert.ok(c.timeCenteredPressure&&m.cutPressure);assert.equal(!!i,mode==='implicit');
  assert.equal(s.auditedFrames+s.auditedIdleFrames+s.invalid,0);assert.equal(s.projectionCalls,600);
  assert.equal(m.multigrid.exhaustedSolves+b.invalid,0);
  if(i){
    assert.equal(i.steps,600);assert.equal(i.invalid+i.exhaustedSteps+i.auditedFrames+i.auditedIdleFrames,0);
    assert.ok(i.peakResidual<=2.01e-10);assert.equal(c.bulkInClosedCellsM3,0);
  }else assert.equal(b.phaseLimiter.exhaustedSteps,0);
  profiles.push({mode,repeat,rawMs:r.medianMs[5],fluidMs:r.medianMs[6],pressureMeanMs:m.meanMs,
    transportMeanMs:i?.meanFrameMs??b.phaseLimiter.meanFrameMs});
}
const median=a=>a.toSorted((a,b)=>a-b)[a.length>>1],performance={};
if(profiled)for(const mode of ['reference','implicit'])performance[mode]=Object.fromEntries(
  ['rawMs','fluidMs','pressureMeanMs','transportMeanMs'].map(key=>[key,median(profiles.filter(r=>r.mode===mode).map(r=>r[key]))]));
const hash=createHash('sha256');for(const n of names){hash.update(n);hash.update(readFileSync(join(shaders,n)));}
console.log(JSON.stringify({schema:1,date:new Date().toISOString(),baseCommit:'77a697f',gpu:'NVIDIA RTX 5090',
  nodeTests:174,windowsCTests:7,scope:'Opt-in implicit coarse transport and partial closing-cell pressure support; no flowing particle/grid ownership.',
  transportAuditsPassed:true,defaultPromotionReady:false,boundedVof:false,profiled,temporalPreservationTested:false,
  cases,comparisons,profiles,performance,remainingAcceptanceFailures:failures,
  caveats:['Conservative positive backward-Euler transport does not guarantee the free-surface upper bound.',
    'This inventory is not added to particle mass or used as a second rendered surface.',
    'The independently reconstructed matrix snapshot audits the final substep of each active frame; GPU residual/positivity counters cover every substep.',
    'Timing fields from validation runs are not performance measurements.',
    'Profile medians exclude the first 32 frames; helper mean timings include warmup. Reference helper is the explicit phase limiter, not the complete explicit transport update.',
    'No new GBV-clean claim; the earlier Windows Graphics Tools probe failed with 0x887A002D.'],
  exeSha256:createHash('sha256').update(readFileSync(exe)).digest('hex'),shaderSetSha256:hash.digest('hex')},null,2));
