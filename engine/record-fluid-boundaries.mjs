// Read existing bounded reports only. Reject stale evidence from older binaries.
import {readFileSync,readdirSync,statSync} from 'node:fs';
import {join} from 'node:path';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
const directory=process.argv[2];
if(!directory)throw new Error('Usage: node engine/record-fluid-boundaries.mjs <runtime directory>');
const exe=join(directory,'NVMatrixFluidLab.exe'),built=statSync(exe).mtimeMs;
const read=name=>{
  const path=join(directory,`${name}.json`);
  assert.ok(statSync(path).mtimeMs>=built,`Stale report: ${name}`);
  return JSON.parse(readFileSync(path,'utf8'));
};
const names=['reference','reference-long','reference-wake','reference-uniform-wake','empty',
  'calm','cycle','calm-long','wake','fall','room','multigrid','bulk','fg','pt'];
const reports=Object.fromEntries(names.map(n=>[n,read(`interior-${n}`)]));
const cases=names.map(name=>{
  const r=reports[name],f=r.fluid,b=f.interior;
  assert.ok(f.validated&&f.maxPostStepDensityAuditError<=.0002);
  assert.equal(f.particleMassUnits+(b?.massUnits??0),f.restMassUnits);
  assert.equal(r.fluidProbes.badRoots+r.fluidProbes.truncated+r.photonCounters[5]+r.photonCounters[6],0);
  if(!name.startsWith('reference'))assert.ok(b.validated&&r.complexity.validated&&!b.invalid);
  return {name,frames:r.frames,steps:f.steps,samples:f.active,mass:f.restMassUnits,
    owners:b?.ownedCells??0,ownerMass:b?.massUnits??0,restorations:b?.promotions??0,
    preDensity:f.maxRelativeDensity,postDensity:f.postStepMaxRelativeDensity,
    peak:f.postStepDensityPeak,auditError:f.maxPostStepDensityAuditError,
    repaired:f.densityRepairRequestedLastSubstep,meanHeight:f.meanHeight,particleMeanHeight:f.particleMeanHeight,
    volume:r.fluidSurface.tetrahedralVolumeEstimate,penetration:f.maxSolidPenetration};
});
const comparisons={};
for(const name of ['calm','calm-long','cycle','wake']){
  const a=reports[name],b=reports[name==='wake'?'reference-uniform-wake':name==='calm-long'?'reference-long':'reference'];
  const volumeError=Math.abs(a.fluidSurface.tetrahedralVolumeEstimate/b.fluidSurface.tetrahedralVolumeEstimate-1);
  const preDelta=a.fluid.maxRelativeDensity-b.fluid.maxRelativeDensity;
  const postDelta=a.fluid.postStepMaxRelativeDensity-b.fluid.postStepMaxRelativeDensity;
  assert.ok(volumeError<=(name==='wake'?.01:.0025)&&preDelta<=.01&&postDelta<=.01,`${name} reference failed`);
  comparisons[name]={volumeError,preDelta,postDelta};
}
assert.ok(reports.calm.fluid.interior.ownedCells&&!reports.calm.fluid.densityRepairRequestedLastSubstep);
assert.ok(reports.wake.fluid.interior.promotions&&reports.wake.fluid.densityRepairRequestedLastSubstep);
assert.ok(reports.wake.fluid.postStepMaxRelativeDensity<=1.05);
assert.equal(reports.cycle.fluid.active,reports.cycle.fluid.particles);
assert.ok(reports.fg.frameGeneration.enabled&&reports.pt.restirPT);
const ordinary=['constant','affine','compression','roundtrip','controls','apic-long'].map(name=>{
  const r=read(`fluid-${name}`);assert.ok(r.fluid.validated);
  return {name,steps:r.fluid.steps,particles:r.fluid.active,postDensity:r.fluid.postStepMaxRelativeDensity};
});
const optical=['colliders','surface-controls'].map(name=>{
  const r=read(`fluid-render-${name}`);assert.ok(r.fluid.validated);
  assert.equal(r.fluidProbes.badRoots+r.fluidProbes.truncated+r.photonCounters[5]+r.photonCounters[6],0);
  return {name,frames:r.frames,roots:r.fluidProbes.hits};
});
const temporal=[128,176,224,272,320].map(frame=>{
  const r=read(`water-temporal-boundaries-${frame}`);
  assert.equal(r.rrHistoryResets,1);assert.equal(r.frames,frame);assert.equal(r.dlssEvaluations,frame);
  return {frame,rrResets:r.rrHistoryResets,atlasResets:r.historyResets};
});
const median=values=>{
  const a=values.toSorted((x,y)=>x-y),i=a.length>>1;
  return a.length%2?a[i]:(a[i-1]+a[i])/2;
};
const runs=[1,2,3].map(i=>read(`room-perf-boundaries-final-orbit-${i}`));
for(const r of runs)assert.ok(r.frames===300&&r.sampleCount===268&&!r.fluid.validated&&!r.frameGeneration.enabled);
const samples=runs.flatMap(r=>r.samplesMs),times=samples.map(s=>s[5]).sort((a,b)=>a-b);
const profile={scene:'room, inlet, foam, orbit; 1080p Balanced; FG off; no invariant snapshots',
  measuredFrames:samples.length,rawFrameMs:median(times),p95Ms:times[Math.floor(.95*(times.length-1))],
  simulationMs:median(samples.map(s=>s[6])),reconstructionMs:median(samples.map(s=>s[7])),
  runMedians:runs.map(r=>r.medianMs[5]),note:'Current operating point, not a controlled old/new speedup comparison'};
const shaderHash=createHash('sha256');
for(const name of readdirSync(join(directory,'shaders')).filter(n=>n.endsWith('.dxil')).sort()){
  shaderHash.update(name);shaderHash.update(readFileSync(join(directory,'shaders',name)));
}
console.log(JSON.stringify({schema:1,date:'2026-09-12',baseCommit:'7f89656',gpu:'NVIDIA RTX 5090',
  scope:'Substep-aligned boundaries and bounded GPU error-triggered density repair; full adaptive goal incomplete',
  scopedGpuChecksPassed:true,cases,comparisons,ordinary,optical,temporal,profile,
  exeSha256:createHash('sha256').update(readFileSync(exe)).digest('hex'),shaderSetSha256:shaderHash.digest('hex'),
  debugLayer:{gpuBasedValidationExecuted:false,previousInitializationError:'0x887A002D'}},null,2));
