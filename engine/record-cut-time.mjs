// Read-only evidence collector. Persist the result with apply_patch.
import {readFileSync,readdirSync,statSync} from 'node:fs';
import {join} from 'node:path';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
import {readCapture} from './validate-captures.mjs';
const folder=process.argv[2];
if(!folder)throw new Error('Usage: node engine/record-cut-time.mjs RUNTIME');
const exe=join(folder,'NVMatrixFluidLab.exe'),shaders=join(folder,'shaders');
const names=readdirSync(shaders).filter(n=>n.endsWith('.dxil')).sort();
const built=Math.max(statSync(exe).mtimeMs,...names.map(n=>statSync(join(shaders,n)).mtimeMs));
function read(name){const path=join(folder,name+'.json');assert.ok(statSync(path).mtimeMs>=built,`Stale ${name}`);return JSON.parse(readFileSync(path));}
function check(r,time,relaxation=false){
  const f=r.fluid,c=f.cutCells,m=f.adaptiveMac;
  assert.ok(f.validated&&m.validated&&c.validated&&m.cutPressure);
  assert.equal(c.timeCenteredPressure,time);assert.equal(m.invalid+c.invalid+c.maxHistoryError,0);
  assert.ok(c.maxTimeAreaError<=1e-4);assert.equal(c.auditedFrames,r.frames);
  assert.equal(r.photonCounters[5]+r.photonCounters[6]+r.fluidProbes.badRoots+r.fluidProbes.truncated,0);
  if(!relaxation){assert.ok(m.multigrid.validated);assert.equal(m.multigrid.exhaustedSolves,0);assert.ok(m.multigrid.peakFinalDivergence<=.000101&&m.preciseMatrixError<=1e-10);}
  return {steps:f.steps,timeAreaError:c.maxTimeAreaError,pressurePeakDivergence:m.multigrid?.peakFinalDivergence,
    matrixError:m.matrixError,preciseMatrixError:m.preciseMatrixError,fluxError:m.fluxError,
    closingSamples:m.auditedClosingCellSamples,closingLiquidSamples:m.auditedClosingLiquidSamples,
    closingConnectedSamples:m.auditedClosingConnectedSamples,closingLiquidSampleVolumeM3:m.auditedClosingLiquidVolumeM3,
    particleDensity:f.postStepMaxRelativeDensity,particleCfl:f.maxParticleCfl,particleMass:f.restMassUnits,
    surfaceVolume:r.fluidSurface.tetrahedralVolumeEstimate};
}
const pressureFrames={empty:4,'room-short':8,calm:120,interior:120,cycle:120,fall:120,room:120,wake:180,controls:12,relaxation:12,adaptive:64};
const pressureCases=Object.entries(pressureFrames).map(([name,frames])=>{
  const r=read('cut-time-'+name);assert.equal(r.frames,frames);const result=check(r,true,name==='relaxation');
  if(['calm','interior','cycle','wake'].includes(name))assert.ok(result.particleDensity<=1.05);
  return {name,frames,...result,optical:readCapture(join(folder,'cut-time-'+name)).summary};
});
const bulkFrames={'initial-calm':1,'initial-room':1,empty:4,short:8,calm:120,fall:90,room:120,wake:180,controls:12,reset:180,adaptive:64,overlay:32};
const bulkCases=Object.entries(bulkFrames).map(([name,frames])=>{
  const r=read(`bulk-bounded-${name}-time`),b=r.fluid.bulk,l=b.phaseLimiter,a=b.sourceAllocation;
  assert.equal(r.frames,frames);const physics=check(r,true);
  assert.ok(b.validated&&a.validated&&(l.steps===0||l.validated));assert.equal(b.steps,r.fluid.steps);assert.equal(l.steps,b.steps);
  assert.equal(b.invalid+l.invalid+l.exhaustedSteps+a.invalid,0);
  assert.ok(b.relativeVolumeError<.0002&&l.peakNewExcessM3<1e-7&&a.newExcessM3<=1e-7);
  if(name==='reset'){
    assert.ok(physics.closingLiquidSamples>0,'No real disappearing liquid pressure cells exercised');
    assert.equal(physics.closingConnectedSamples,physics.closingLiquidSamples,'Closing liquid lost every pressure outlet');
  }
  return {name,frames,...physics,volumeM3:b.volumeM3,massError:b.relativeVolumeError,
    pendingM3:a.pendingVolumeM3,excessM3:b.excessVolumeM3,closedM3:r.fluid.cutCells.bulkInClosedCellsM3,
    phasePeakNewExcessM3:l.peakNewExcessM3,phaseIterations:l.iterations,
    optical:readCapture(join(folder,`bulk-bounded-${name}-time`)).summary};
});
const comparisons=['calm','room','wake','adaptive'].map(name=>{
  const a=read(`bulk-bounded-${name}`),b=read(`bulk-bounded-${name}-time`),before=check(a,false),after=check(b,true);
  assert.equal(a.frames,b.frames);assert.equal(before.particleMass,after.particleMass);
  assert.ok(after.particleDensity<=before.particleDensity+.01,`${name}: paired particle compression regressed`);
  assert.ok(Math.abs(after.surfaceVolume/before.surfaceVolume-1)<=.01,`${name}: paired surface volume changed`);
  return {name,before,after,beforeExcessM3:a.fluid.bulk.excessVolumeM3,afterExcessM3:b.fluid.bulk.excessVolumeM3,
    beforeClosedM3:a.fluid.cutCells.bulkInClosedCellsM3,afterClosedM3:b.fluid.cutCells.bulkInClosedCellsM3,
    densityDifference:after.particleDensity-before.particleDensity,
    surfaceVolumeRelativeDifference:Math.abs(after.surfaceVolume/before.surfaceVolume-1)};
});
const profiles=[];
for(let repeat=1;repeat<=3;repeat++)for(const mode of ['endpoint','time']){
  const r=read(`cut-time-cost-${mode}-${repeat}`),c=r.fluid.cutCells,b=r.fluid.bulk,m=r.fluid.adaptiveMac;
  assert.equal(r.frames,300);assert.equal(r.sampleCount,268);assert.equal(r.fluid.steps,600);
  assert.ok(!r.frameGeneration.enabled&&!r.fluid.validated&&!c.auditedFrames&&!m.auditedFrames&&!b.validated);
  assert.equal(c.timeCenteredPressure,mode==='time');assert.equal(m.multigrid.exhaustedSolves+b.phaseLimiter.exhaustedSteps+b.invalid,0);
  profiles.push({mode,repeat,rawMs:r.medianMs[5],fluidMs:r.medianMs[6],cutMeanMs:c.meanFrameMs,pressureMeanMs:m.meanMs,cutGpuBytes:c.allocatedGpuBufferBytes});
}
const median=a=>a.toSorted((a,b)=>a-b)[a.length>>1],performance={};
for(const mode of ['endpoint','time'])performance[mode]=Object.fromEntries(['rawMs','fluidMs','cutMeanMs','pressureMeanMs','cutGpuBytes'].map(key=>[key,median(profiles.filter(r=>r.mode===mode).map(r=>r[key]))]));
const hash=createHash('sha256');for(const n of names){hash.update(n);hash.update(readFileSync(join(shaders,n)));}
console.log(JSON.stringify({schema:1,date:new Date().toISOString(),baseCommit:'0c9cf66',gpu:'NVIDIA RTX 5090',nodeTests:160,windowsCTests:7,
  scope:'Opt-in time-centered cut pressure. Not complete swept-solid geometry, closing-cell transport or flowing Narrow Band FLIP.',
  defaultPromotionReady:false,pressureCases,bulkCases,comparisons,profiles,performance,
  caveats:['Pressure residuals use endpoint-mean temporal support volume, not end-state volume.',
    'Linear endpoint SDF interpolation can miss intermediate occlusions or fast/subgrid motion.',
    'Closing-cell counters are final-substep audit sample sums, not all-substep or unique-cell counts.',
    'Positive pressure support alone does not remove the explicit 95% phase donor cap or supply authoritative bulk liquid support.',
    'Particle compression, optical temporal preservation and raw cost are separate acceptance checks.',
    'No new GBV-clean claim: prior Windows Graphics Tools initialization failed with 0x887A002D.'],
  exeSha256:createHash('sha256').update(readFileSync(exe)).digest('hex'),shaderSetSha256:hash.digest('hex')},null,2));
