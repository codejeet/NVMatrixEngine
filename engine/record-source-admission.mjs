// Read-only evidence collector. Persist output with apply_patch.
import {readFileSync,readdirSync,statSync} from 'node:fs';
import {join} from 'node:path';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
const folder=process.argv[2];
if(!folder)throw new Error('Usage: node engine/record-source-admission.mjs RUNTIME');
const exe=join(folder,'NVMatrixFluidLab.exe'),shaders=join(folder,'shaders');
const names=readdirSync(shaders).filter(n=>n.endsWith('.dxil')).sort();
const built=Math.max(statSync(exe).mtimeMs,...names.map(n=>statSync(join(shaders,n)).mtimeMs));
function read(name){const p=join(folder,name+'.json');assert.ok(statSync(p).mtimeMs>=built,`Stale ${name}`);return JSON.parse(readFileSync(p));}
const expected={'initial-calm':1,'initial-room':1,empty:4,short:8,calm:120,fall:90,room:120,wake:180,controls:12,reset:180,adaptive:64,overlay:32};
const cases=Object.entries(expected).map(([name,frames])=>{
  const r=read(`bulk-capacity-${name}`),b=r.fluid.bulk,a=b.sourceAllocation,m=r.fluid.adaptiveMac;
  assert.equal(r.frames,frames);assert.ok(b.projectedFlux&&b.validated&&a.validated&&m.validated&&r.fluid.cutCells.validated&&r.fluid.validated);
  assert.equal(b.invalid+a.invalid+m.invalid+r.fluid.cutCells.invalid,0);
  assert.ok(b.relativeVolumeError<.0002&&a.newExcessM3<=1e-7&&a.relativeConservationError<2e-5);
  assert.ok(m.multigrid.validated&&!m.multigrid.exhaustedSolves&&m.multigrid.peakFinalDivergence<.000101);
  assert.equal(r.photonCounters[5]+r.photonCounters[6]+r.fluidProbes.badRoots+r.fluidProbes.truncated,0);
  if(name.startsWith('initial-'))assert.ok(a.admittedLastFrameM3>0&&a.iterations>0&&a.snapshotMaxError>0&&b.excessVolumeM3<1e-7);
  if(name==='calm')assert.ok(b.excessVolumeM3<1e-7&&m.coarseLeaves>0);
  if(name==='controls')assert.ok(!a.advanced&&a.iterations===0&&a.admittedLastFrameM3===0&&r.fluid.steps===0);
  return {name,frames,steps:b.steps,totalVolumeM3:b.volumeM3,pendingM3:a.pendingVolumeM3,
    admittedLastFrameM3:a.admittedLastFrameM3,routedLastFrameM3:a.routedLastFrameM3,
    sourceNewExcessM3:a.newExcessM3,postAdvectionExcessM3:b.excessVolumeM3,
    sourceSnapshotMaxError:a.snapshotMaxError,sourceConservationError:a.relativeConservationError,
    sourceEnergyIncrease:a.energyIncrease,totalMassError:b.relativeVolumeError,
    pendingCells:a.pendingCells,unroutableCells:a.unroutableCells,iterations:a.iterations,
    routingToleranceM3:a.routingToleranceM3,advanced:a.advanced,peakDivergence:m.multigrid.peakFinalDivergence};
});
const comparisons=['initial-calm','initial-room','calm','room','wake','adaptive'].map(name=>{
  const a=read(`bulk-projected-${name}`),b=read(`bulk-capacity-${name}`);
  assert.ok(!a.fluid.bulk.sourceAllocation&&a.fluid.bulk.validated);
  assert.equal(a.fluid.restMassUnits,b.fluid.restMassUnits);
  return {name,beforeResidentExcessM3:a.fluid.bulk.excessVolumeM3,
    afterResidentExcessM3:b.fluid.bulk.excessVolumeM3,pendingM3:b.fluid.bulk.sourceAllocation.pendingVolumeM3,
    closedResidentM3:b.fluid.cutCells.bulkInClosedCellsM3};
});
const profiles=[];
for(let repeat=1;repeat<=3;repeat++)for(const mode of ['off','on']){
  const r=read(`source-admission-cost-${mode}-${repeat}`),b=r.fluid.bulk,a=b.sourceAllocation,m=r.fluid.adaptiveMac;
  assert.equal(r.frames,300);assert.equal(r.sampleCount,268);assert.equal(r.fluid.steps,600);
  assert.ok(!r.frameGeneration.enabled&&!r.fluid.validated&&!b.validated&&!m.auditedFrames&&m.cutPressure&&!m.multigrid.exhaustedSolves);
  assert.equal(r.photonCounters[5]+r.photonCounters[6]+b.invalid,0);
  if(mode==='on')assert.ok(a&&!a.validated&&!a.invalid&&a.newExcessM3<=1e-7);else assert.ok(!a);
  profiles.push({mode,repeat,rawMs:r.medianMs[5],fluidMs:r.medianMs[6],bulkMeanMs:b.meanFrameMs,
    sourceMeanMs:a?.meanFrameMs??0,sourceGpuBytes:a?.allocatedGpuBufferBytes??0});
}
const median=a=>a.toSorted((a,b)=>a-b)[a.length>>1],performance={};
for(const mode of ['off','on']){
  const p=profiles.filter(p=>p.mode===mode);
  performance[mode]=Object.fromEntries(['rawMs','fluidMs','bulkMeanMs','sourceMeanMs'].map(key=>[key,median(p.map(r=>r[key]))]));
}
const hash=createHash('sha256');for(const n of names){hash.update(n);hash.update(readFileSync(join(shaders,n)));}
console.log(JSON.stringify({schema:1,date:new Date().toISOString(),baseCommit:'766d64f',gpu:'NVIDIA RTX 5090',
  scope:'Capacity-bounded initial/inlet admission with explicit pending mass and momentum; not bounded advection or authoritative bulk ownership.',
  defaultPromotionReady:false,nodeTests:146,windowsCTests:7,cases,comparisons,profiles,performance,
  caveats:['Initial and resident-only excess excludes pending requests; total accounting includes both.',
    'Pending is not dropped when an iteration/error budget is reached. Later advection and closed-cell excess remain visible.',
    'The sampled coarse graph does not encode disconnected sub-cell components; boundary refinement/ownership remains required.',
    'No new GPU-validation claim: prior Windows Graphics Tools initialization failed with 0x887A002D; no OS/driver changes made.',
    'Exact optical isolation is checked separately with deterministic bins and fixed-point photons.'],
  exeSha256:createHash('sha256').update(readFileSync(exe)).digest('hex'),shaderSetSha256:hash.digest('hex')},null,2));
