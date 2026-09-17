// Read-only evidence collector. Pipe the emitted JSON through the editor when
// checkpointing; no stale reports or running-process timings are accepted.
import {readFileSync,readdirSync,statSync} from 'node:fs';
import {join} from 'node:path';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
const directory=process.argv[2];
if(!directory)throw new Error('Usage: node engine/record-fluid-mac.mjs <runtime directory>');
const exe=join(directory,'NVMatrixFluidLab.exe'),built=statSync(exe).mtimeMs;
const read=name=>{
  const path=join(directory,`${name}.json`);
  assert.ok(statSync(path).mtimeMs>=built,`Stale report: ${name}`);
  return JSON.parse(readFileSync(path,'utf8'));
};
const names=['reference-calm','reference-fall','reference-wake','reference-room',
  'empty','audit','view','calm','cycle','fall','wake','interior','room','odd','fg','pt'];
const reports=Object.fromEntries(names.map(n=>[n,read(`mac-${n}`)]));
const cases=names.map(name=>{
  const r=reports[name],f=r.fluid,m=f.adaptiveMac;
  assert.ok(f.validated&&f.deterministicBins&&r.complexity.validated&&f.maxPostStepDensityAuditError<=.0002);
  assert.equal(f.particleMassUnits+(f.interior?.massUnits??0),f.restMassUnits);
  assert.equal(r.fluidProbes.badRoots+r.fluidProbes.truncated+r.photonCounters[5]+r.photonCounters[6],0);
  if(!name.startsWith('reference'))assert.ok(m.validated&&!m.invalid&&m.matrixError<=.00002&&
    m.rhsDivergenceError<=.00002&&m.fluxError<=.00004&&m.residualMismatch<=.0002&&m.prolongationError<=.0002);
  return {name,frames:r.frames,steps:f.steps,mass:f.restMassUnits,samples:f.active,
    postDensity:f.postStepMaxRelativeDensity,volume:r.fluidSurface.tetrahedralVolumeEstimate,
    divergenceBefore:f.divergenceRmsBefore,divergenceAfter:f.divergenceRmsAfter,
    operator:m??null};
});
const comparisons={};
for(const name of ['calm','cycle','fall','wake','room']){
  const a=reports[name],b=reports[`reference-${name==='cycle'?'calm':name}`];
  const volumeError=Math.abs(a.fluidSurface.tetrahedralVolumeEstimate/b.fluidSurface.tetrahedralVolumeEstimate-1);
  const densityDelta=a.fluid.postStepMaxRelativeDensity-b.fluid.postStepMaxRelativeDensity;
  assert.ok(volumeError<=.01&&densityDelta<=.01);
  comparisons[name]={volumeError,densityDelta};
}
for(const name of ['calm','cycle','interior'])assert.ok(reports[name].fluid.adaptiveMac.coarseLeaves>0);
assert.ok(reports.cycle.fluid.adaptiveMac.finePromotions>0&&reports.cycle.fluid.adaptiveMac.coarseDemotions>96);
assert.ok(reports.wake.fluid.adaptiveMac.peakCoarseLeaves>0&&reports.wake.fluid.adaptiveMac.finePromotions>0);
for(const name of ['calm','cycle','wake','interior'])assert.ok(reports[name].fluid.postStepMaxRelativeDensity<=1.05);
assert.equal(reports.odd.fluid.adaptiveMac.iterations,121);
assert.ok(reports.fg.frameGeneration.enabled&&reports.pt.restirPT);
const temporal=[128,176,224,272,320].map(frame=>{
  const r=read(`water-temporal-mac-${frame}`),b=read(`water-temporal-mac-reference-${frame}`);
  assert.equal(r.rrHistoryResets,1);assert.equal(b.rrHistoryResets,1);
  assert.equal(r.frames,frame);assert.equal(r.dlssEvaluations,frame);
  return {frame,rrResets:r.rrHistoryResets,coarseLeaves:r.fluid.adaptiveMac.coarseLeaves};
});
const profile={};
for(const mode of ['reference','adaptive']){
  const r=read(`room-perf-mac-${mode}-orbit-1`);
  assert.ok(r.frames===300&&r.sampleCount===268&&!r.fluid.validated&&!r.frameGeneration.enabled&&!r.fluid.deterministicBins);
  const times=r.samplesMs.map(s=>s[5]).sort((a,b)=>a-b);
  profile[mode]={medianRawMs:r.medianMs[5],p95RawMs:times[Math.floor(.95*(times.length-1))],
    medianSimulationMs:r.medianMs[6],measuredFrames:r.sampleCount,
    peakCoarseLeaves:r.fluid.adaptiveMac?.peakCoarseLeaves??0};
}
profile.scope='One sequential same-build 1080p Balanced room/inlet/foam/orbit run per mode; FG off, no invariant snapshots or deterministic sorting. Operating-cost check, not statistically established speedup.';
const ordinary=['constant','affine','compression','roundtrip','controls','apic-long'].map(name=>{
  const r=read(`fluid-${name}`);assert.ok(r.fluid.validated&&!r.fluid.adaptiveMac&&!r.fluid.deterministicBins);
  return {name,steps:r.fluid.steps,particles:r.fluid.active};
});
const optical=['colliders','surface-controls'].map(name=>{
  const r=read(`fluid-render-${name}`);assert.ok(r.fluid.validated);
  assert.equal(r.fluidProbes.badRoots+r.fluidProbes.truncated+r.photonCounters[5]+r.photonCounters[6],0);
  return {name,frames:r.frames,roots:r.fluidProbes.hits};
});
const hash=createHash('sha256');
for(const name of readdirSync(join(directory,'shaders')).filter(n=>n.endsWith('.dxil')).sort()){
  hash.update(name);hash.update(readFileSync(join(directory,'shaders',name)));
}
console.log(JSON.stringify({schema:1,date:'2026-09-13',baseCommit:'0cef4c5',gpu:'NVIDIA RTX 5090',
  scope:'Opt-in coupled two-level MAC projection; dense transfer cache remains; full adaptive goal incomplete',
  scopedGpuChecksPassed:true,cases,comparisons,ordinary,optical,temporal,profile,
  exeSha256:createHash('sha256').update(readFileSync(exe)).digest('hex'),shaderSetSha256:hash.digest('hex'),
  debugLayer:{gpuBasedValidationExecuted:false,previousInitializationError:'0x887A002D'}},null,2));
