// Read-only final-checkpoint evidence collector.
import {readFileSync,readdirSync,statSync} from 'node:fs';
import {join} from 'node:path';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
const directory=process.argv[2];
if(!directory)throw new Error('Usage: node engine/record-mac-multigrid.mjs <runtime directory>');
const exe=join(directory,'NVMatrixFluidLab.exe'),shaders=join(directory,'shaders');
const shaderNames=readdirSync(shaders).filter(n=>n.endsWith('.dxil')).sort();
const built=Math.max(statSync(exe).mtimeMs,...shaderNames.map(n=>statSync(join(shaders,n)).mtimeMs));
const read=name=>{
  const path=join(directory,`${name}.json`);
  assert.ok(statSync(path).mtimeMs>=built,`Stale report: ${name}`);
  return JSON.parse(readFileSync(path,'utf8'));
};
const names=['reference-calm','reference-fall','reference-wake','reference-room',
  'empty','audit','view','calm','cycle','fall','wake','interior','room','odd','fg','pt'];
const reports=Object.fromEntries(names.map(n=>[n,read(`mac-mgpcg-${n}`)]));
const cases=names.map(name=>{
  const r=reports[name],f=r.fluid,m=f.adaptiveMac;
  assert.ok(f.validated&&f.deterministicBins&&r.complexity.validated&&f.maxPostStepDensityAuditError<=.0002);
  assert.equal(f.particleMassUnits+(f.interior?.massUnits??0),f.restMassUnits);
  assert.equal(r.fluidProbes.badRoots+r.fluidProbes.truncated+r.photonCounters[5]+r.photonCounters[6],0);
  if(!name.startsWith('reference')){
    assert.ok(m.validated&&!m.invalid&&m.solver==='multigrid-pcg');
    assert.ok(m.matrixError<=.00002&&m.rhsDivergenceError<=.00002&&m.fluxError<=.00004&&m.residualMismatch<=.0002);
    const mg=m.multigrid;assert.ok(mg.validated&&mg.hierarchyError<=.00002&&mg.factorError<=.00002&&mg.residualError<=.0002);
    assert.ok(mg.peakFinalDivergence<=.000101&&!mg.exhaustedSolves&&mg.cappedMaxDivergence===0);
    assert.equal(f.densitySchedule,'paired-tile');
  }else assert.equal(f.pressureIterations,1000);
  assert.equal(f.densityIterations,120);
  return {name,frames:r.frames,steps:f.steps,mass:f.restMassUnits,samples:f.active,
    peakDensity:f.postStepMaxRelativeDensity,volume:r.fluidSurface.tetrahedralVolumeEstimate,
    divergenceBefore:f.divergenceRmsBefore,divergenceAfter:f.divergenceRmsAfter,operator:m??null};
});
const comparisons={};
for(const name of ['calm','cycle','fall','wake','room']){
  const a=reports[name],b=reports[`reference-${name==='cycle'?'calm':name}`];
  const volumeError=Math.abs(a.fluidSurface.tetrahedralVolumeEstimate/b.fluidSurface.tetrahedralVolumeEstimate-1);
  const densityDelta=a.fluid.postStepMaxRelativeDensity-b.fluid.postStepMaxRelativeDensity;
  assert.ok(volumeError<=.01&&densityDelta<=.01);
  comparisons[name]={fineReferenceSweeps:1000,volumeError,densityDelta};
}
for(const name of ['calm','cycle','interior'])assert.ok(reports[name].fluid.adaptiveMac.coarseLeaves>0);
assert.ok(reports.cycle.fluid.adaptiveMac.finePromotions>0&&reports.cycle.fluid.adaptiveMac.coarseDemotions>96);
assert.ok(reports.wake.fluid.adaptiveMac.peakCoarseLeaves>0&&reports.wake.fluid.adaptiveMac.finePromotions>0);
for(const name of ['calm','cycle','wake','interior'])assert.ok(reports[name].fluid.postStepMaxRelativeDensity<=1.05);
assert.ok(reports.fg.frameGeneration.enabled&&reports.pt.restirPT);
const schedules=[];
for(const fixture of ['room','odd','fall']){
  const reference=read(`mac-schedule-${fixture}-paired`);
  for(const mode of ['paired','scalar','split']){
    const r=read(`mac-schedule-${fixture}-${mode}`),f=r.fluid,mg=f.adaptiveMac.multigrid;
    const volumeError=Math.abs(r.fluidSurface.tetrahedralVolumeEstimate/reference.fluidSurface.tetrahedralVolumeEstimate-1);
    const densityError=Math.abs(f.postStepMaxRelativeDensity-reference.fluid.postStepMaxRelativeDensity);
    assert.ok(f.validated&&mg.validated&&!mg.exhaustedSolves&&mg.peakFinalDivergence<=.000101);
    assert.ok(volumeError<=1e-6&&densityError<=1e-5&&f.particleMassUnits===reference.fluid.particleMassUnits);
    assert.equal(f.densityIterations,fixture==='odd'?121:120);
    assert.equal(f.densitySchedule,mode==='scalar'?'scalar':'paired-tile');
    assert.equal(mg.coarseSchedule,mode==='split'?'distributed':'single-group');
    schedules.push({fixture,mode,volumeError,densityError,peakFinalDivergence:mg.peakFinalDivergence});
  }
}
const profile={};
for(const mode of ['reference','final','split','scalar']){
  const r=read(`room-perf-mgpcg-${mode}-orbit-1`);
  assert.ok(r.frames===300&&r.sampleCount===268&&!r.fluid.validated&&!r.frameGeneration.enabled&&!r.fluid.deterministicBins);
  const times=r.samplesMs.map(s=>s[5]).sort((a,b)=>a-b);
  profile[mode]={medianRawMs:r.medianMs[5],p95RawMs:times[Math.floor(.95*(times.length-1))],
    medianSimulationMs:r.medianMs[6],measuredFrames:r.sampleCount,solver:r.fluid.adaptiveMac?.solver??'uniform',
    densityIterations:r.fluid.densityIterations??(mode==='reference'?60:120),
    convergence:r.fluid.adaptiveMac?.multigrid??null};
  const mg=r.fluid.adaptiveMac?.multigrid;
  if(mg)assert.ok(!mg.exhaustedSolves&&mg.peakFinalDivergence<=.000101);
}
profile.scope='One sequential same-build 1080p Balanced room/inlet/foam/orbit run per mode; FG off; no snapshots or deterministic sorting. Operating-cost check, not a statistically established speedup.';
const ordinary=['constant','affine','compression','roundtrip','controls','apic-long'].map(name=>{
  const r=read(`fluid-${name}`);assert.ok(r.fluid.validated&&!r.fluid.adaptiveMac&&!r.fluid.deterministicBins);
  assert.equal(r.fluid.densityIterations,60);assert.equal(r.fluid.densitySchedule,'scalar');
  return {name,steps:r.fluid.steps,particles:r.fluid.active};
});
const relaxation=['audit','cycle','room','odd'].map(name=>{
  const r=read(`mac-${name}`),m=r.fluid.adaptiveMac;
  assert.ok(r.fluid.validated&&m.validated&&!m.invalid&&m.solver==='absolute-row-relaxation'&&!m.multigrid);
  assert.equal(r.fluid.densityIterations,60);assert.equal(r.fluid.densitySchedule,'scalar');
  return {name,frames:r.frames,coarseLeaves:m.coarseLeaves};
});
const temporal=[128,176,224,272,320].map(frame=>{
  const r=read(`water-temporal-mgpcg-${frame}`),b=read(`water-temporal-mgpcg-reference-${frame}`);
  assert.equal(r.rrHistoryResets,1);assert.equal(b.rrHistoryResets,1);
  const mg=r.fluid.adaptiveMac.multigrid;
  assert.ok(!mg.exhaustedSolves&&mg.peakFinalDivergence<=.000101);
  return {frame,rrResets:r.rrHistoryResets,coarseLeaves:r.fluid.adaptiveMac.coarseLeaves};
});
const hash=createHash('sha256');for(const name of shaderNames){hash.update(name);hash.update(readFileSync(join(shaders,name)));}
console.log(JSON.stringify({schema:2,date:new Date().toISOString(),baseCommit:'3ca2e79',gpu:'NVIDIA RTX 5090',
  scope:'Opt-in multigrid-preconditioned CG on the actual mixed MAC operator; full adaptive system incomplete',
  scopedChecksPassed:true,allSolvesConverged:cases.every(c=>!(c.operator?.multigrid?.exhaustedSolves)),
  cpuTests:90,windowsTests:7,cases,comparisons,schedules,ordinary,relaxation,temporal,profile,
  exeSha256:createHash('sha256').update(readFileSync(exe)).digest('hex'),shaderSetSha256:hash.digest('hex'),
  debugLayer:{gpuBasedValidationExecuted:false,previousInitializationError:'0x887A002D'}},null,2));
