// Read-only report collector. Persist output with apply_patch.
import {readFileSync,readdirSync,statSync} from 'node:fs';
import {join} from 'node:path';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
const folder=process.argv[2];
if(!folder)throw new Error('Usage: node engine/record-bulk-projected.mjs RUNTIME');
const exe=join(folder,'NVMatrixFluidLab.exe'),shaders=join(folder,'shaders');
const names=readdirSync(shaders).filter(n=>n.endsWith('.dxil')).sort();
const built=Math.max(statSync(exe).mtimeMs,...names.map(n=>statSync(join(shaders,n)).mtimeMs));
function read(name){const p=join(folder,name+'.json');assert.ok(statSync(p).mtimeMs>=built,`Stale ${name}`);return JSON.parse(readFileSync(p));}
const expected={empty:4,short:8,calm:120,fall:90,room:120,wake:180,controls:12,reset:180,adaptive:64,overlay:32};
const cases=Object.entries(expected).map(([name,frames])=>{
  const r=read(`bulk-projected-${name}`),b=r.fluid.bulk,m=r.fluid.adaptiveMac,c=r.fluid.cutCells;
  assert.equal(r.frames,frames);assert.ok(b.projectedFlux&&b.validated&&m.validated&&m.cutPressure&&c.validated&&r.fluid.validated);
  assert.equal(b.invalid+m.invalid+c.invalid,0);assert.ok(b.relativeVolumeError<.0002);
  assert.ok(b.fluxRestrictionMaxError<1e-10&&b.capacityRestrictionMaxError<1e-7&&Number.isFinite(b.forceSourceMaxError));
  assert.ok(m.multigrid.validated&&!m.multigrid.exhaustedSolves&&m.multigrid.peakFinalDivergence<.000101);
  assert.equal(r.photonCounters[5]+r.photonCounters[6]+r.fluidProbes.badRoots+r.fluidProbes.truncated,0);
  assert.equal(b.steps,r.fluid.steps);
  return {name,frames,steps:b.steps,volumeM3:b.volumeM3,relativeMassError:b.relativeVolumeError,
    momentumErrorKgMps:b.momentumErrorKgMps,fluxRestrictionError:b.fluxRestrictionMaxError,
    capacityRestrictionError:b.capacityRestrictionMaxError,forceSourceError:b.forceSourceMaxError,
    excessM3:b.excessVolumeM3,inClosedCellsM3:c.bulkInClosedCellsM3,maxFraction:b.maxVolumeFraction,overfilledCells:b.overfilledCells,
    limitedDonorsLastFrame:b.limitedDonorUpdates,coarseLeaves:m.coarseLeaves,
    peakDivergence:m.multigrid.peakFinalDivergence,sourceCalls:b.sourceCalls,resets:b.resets};
});
const localCapacityComparisons=['calm','room','wake','adaptive'].map(name=>{
  const a=read(`bulk-legacy-${name}`),b=read(`bulk-projected-${name}`);
  assert.ok(a.fluid.bulk.validated&&b.fluid.bulk.validated);
  // The legacy bulk report uses full boxes; compare BOTH against the common
  // cut-cell capacity observer instead, including any increase in total excess.
  return {name,legacyExcessM3:a.fluid.cutCells.bulkExcessCapacityM3,
    projectedExcessM3:b.fluid.cutCells.bulkExcessCapacityM3,
    legacyClosedM3:a.fluid.cutCells.bulkInClosedCellsM3,
    projectedClosedM3:b.fluid.cutCells.bulkInClosedCellsM3};
});
const legacyFixtures=['translation','high-cfl','persistent'].map(name=>{
  const r=read(`bulk-${name}`),b=r.fluid.bulk;
  assert.ok(b.validated&&!b.projectedFlux&&!b.invalid&&b.relativeVolumeError<.0002);
  if(name==='high-cfl')assert.ok(b.limitedDonorUpdates>0);
  return {name,frames:r.frames,steps:b.steps,relativeMassError:b.relativeVolumeError};
});
const profiles=[];
for(let repeat=1;repeat<=3;repeat++)for(const mode of ['off','on']){
  const r=read(`bulk-projected-cost-${mode}-${repeat}`),m=r.fluid.adaptiveMac,b=r.fluid.bulk;
  assert.equal(r.frames,300);assert.equal(r.sampleCount,268);assert.equal(r.fluid.steps,600);
  assert.ok(!r.frameGeneration.enabled&&!r.fluid.validated&&!m.auditedFrames&&m.cutPressure&&!m.multigrid.exhaustedSolves);
  assert.equal(r.photonCounters[5]+r.photonCounters[6],0);
  if(mode==='on')assert.ok(b.projectedFlux&&!b.validated&&!b.invalid&&b.relativeVolumeError<.0002);
  else assert.ok(!b);
  profiles.push({mode,repeat,rawMs:r.medianMs[5],fluidMs:r.medianMs[6],bulkMeanMs:b?.meanFrameMs??0,bulkGpuBytes:b?.allocatedGpuBufferBytes??0});
}
const median=a=>a.toSorted((a,b)=>a-b)[a.length>>1],performance={};
for(const mode of ['off','on']){
  const p=profiles.filter(p=>p.mode===mode);
  performance[mode]=Object.fromEntries(['rawMs','fluidMs','bulkMeanMs'].map(key=>[key,median(p.map(r=>r[key]))]));
}
const hash=createHash('sha256');for(const n of names){hash.update(n);hash.update(readFileSync(join(shaders,n)));}
console.log(JSON.stringify({schema:1,date:new Date().toISOString(),baseCommit:'912c8b0',gpu:'NVIDIA RTX 5090',
  scope:'Canonical projected bulk flux/capacity integration. Passive replica only; not bounded VOF or authoritative flowing ownership.',
  defaultPromotionReady:false,nodeTests:138,windowsCTests:7,cases,localCapacityComparisons,legacyFixtures,profiles,performance,
  gpuValidation:{available:false,lastAttemptCommit:'912c8b0',initializationError:'0x887A002D',retriedThisCheckpoint:false},
  caveats:['Mass/flux audits do not certify local bounded liquid support. Excess remains visible in these reports.',
    'Zero-capacity diagnostic fraction uses a 1e-20 denominator floor; no such floor is used for transport.',
    'Optical isolation is checked separately by check-bulk-projected.mjs with deterministic bins/fixed-point photons.',
    'Default launchers and particle/optical ownership are unchanged. No completed adaptive-physics or performance claim.',
    'Windows Graphics Tools remain unavailable from the prior checkpoint; no GBV-clean claim.'],
  exeSha256:createHash('sha256').update(readFileSync(exe)).digest('hex'),shaderSetSha256:hash.digest('hex')},null,2));
