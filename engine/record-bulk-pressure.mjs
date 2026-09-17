// Read-only evidence collector. Persist its output with apply_patch.
import {readFileSync,readdirSync,statSync} from 'node:fs';
import {join} from 'node:path';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
import {readCapture} from './validate-captures.mjs';
const folder=process.argv[2];
const validationOnly=process.argv[3]==='--validation-only';
if(!folder||(process.argv[3]&&!validationOnly))throw new Error('Usage: node engine/record-bulk-pressure.mjs RUNTIME [--validation-only]');
const exe=join(folder,'NVMatrixFluidLab.exe'),shaders=join(folder,'shaders');
const names=readdirSync(shaders).filter(n=>n.endsWith('.dxil')).sort();
const built=Math.max(statSync(exe).mtimeMs,...names.map(n=>statSync(join(shaders,n)).mtimeMs));
function read(name){const path=join(folder,name+'.json');assert.ok(statSync(path).mtimeMs>=built,`Stale ${name}`);return JSON.parse(readFileSync(path));}
function capture(name){assert.ok(statSync(join(folder,name+'.inputs')).mtimeMs>=built,`Stale capture ${name}`);return readCapture(join(folder,name)).summary;}
function check(r,support){
  const f=r.fluid,b=f.bulk,c=f.cutCells,m=f.adaptiveMac,l=b.phaseLimiter,a=b.sourceAllocation,s=f.bulkPressure;
  assert.ok(f.validated&&m.validated&&c.validated&&b.validated&&a.validated&&(!l.steps||l.validated));
  assert.ok(c.timeCenteredPressure&&m.cutPressure&&b.projectedFlux&&m.multigrid.validated);
  assert.equal(!!s,support);assert.equal(b.steps,f.steps);assert.equal(l.steps,b.steps);
  assert.equal(m.invalid+c.invalid+b.invalid+l.invalid+l.exhaustedSteps+a.invalid+m.multigrid.exhaustedSolves,0);
  assert.ok(c.maxTimeAreaError<=1e-4&&m.preciseMatrixError<=1e-10&&m.multigrid.peakFinalDivergence<=.000101);
  assert.ok(b.relativeVolumeError<.0002&&l.peakNewExcessM3<=1e-7&&a.newExcessM3<=1e-7);
  assert.equal(r.photonCounters[5]+r.photonCounters[6]+r.fluidProbes.badRoots+r.fluidProbes.truncated,0);
  if(s){
    assert.ok(!s.projectionCalls||s.validated);assert.equal(s.invalid,0);
    assert.equal(s.auditedFrames+s.auditedIdleFrames,r.frames);
    assert.ok(s.velocityReferenceError<=2e-6&&s.weightReferenceError<=2e-6);
    if(!s.lastFrameProjectionCalls)assert.equal(s.lastAddedCells+s.lastFilledFaces+s.lastFilledCellVisits,0);
  }
  return {steps:f.steps,volumeM3:b.volumeM3,massError:b.relativeVolumeError,
    excessM3:b.excessVolumeM3,closedM3:c.bulkInClosedCellsM3,pendingM3:a.pendingVolumeM3,
    phasePeakNewExcessM3:l.peakNewExcessM3,phaseIterations:l.iterations,
    pressurePeakDivergence:m.multigrid.peakFinalDivergence,preciseMatrixError:m.preciseMatrixError,
    closingLiquidSamples:m.auditedClosingLiquidSamples,closingConnectedSamples:m.auditedClosingConnectedSamples,
    particleDensity:f.postStepMaxRelativeDensity,particleCfl:f.maxParticleCfl,particleMass:f.restMassUnits,
    surfaceVolume:r.fluidSurface.tetrahedralVolumeEstimate,coverage:s};
}
const caseFrames={'initial-calm':1,'initial-room':1,empty:4,short:8,calm:120,fall:90,room:120,'room-no-tension':120,wake:180,controls:12,reset:180,adaptive:64,overlay:32};
const cases=Object.entries(caseFrames).map(([name,frames])=>{
  const prefix=`bulk-bounded-${name}-time-support`,r=read(prefix);
  assert.equal(r.frames,frames);const result=check(r,true);
  if(name==='reset'){
    assert.ok(result.closingLiquidSamples>0);assert.equal(result.closingConnectedSamples,result.closingLiquidSamples);
  }
  return {name,frames,...result,optical:capture(prefix)};
});
const comparisons=['calm','room','room-no-tension','wake','adaptive'].map(name=>{
  const a=read(`bulk-bounded-${name}-time`),b=read(`bulk-bounded-${name}-time-support`),before=check(a,false),after=check(b,true);
  assert.equal(a.frames,b.frames);assert.equal(before.particleMass,after.particleMass);
  assert.ok(after.particleDensity<=before.particleDensity+.01,`${name}: paired particle compression regressed`);
  assert.ok(Math.abs(after.surfaceVolume/before.surfaceVolume-1)<=.01,`${name}: paired surface volume changed`);
  return {name,before,after,densityDifference:after.particleDensity-before.particleDensity,
    surfaceVolumeRelativeDifference:Math.abs(after.surfaceVolume/before.surfaceVolume-1),
    referenceOptical:capture(`bulk-bounded-${name}-time`)};
});
const profiles=[];
for(let repeat=1;repeat<=(validationOnly?0:3);repeat++)for(const mode of ['reference','support']){
  const r=read(`bulk-pressure-cost-${mode}-${repeat}`),c=r.fluid.cutCells,b=r.fluid.bulk,m=r.fluid.adaptiveMac,s=r.fluid.bulkPressure;
  assert.equal(r.frames,300);assert.equal(r.sampleCount,268);assert.equal(r.fluid.steps,600);assert.equal(r.fluid.droppedSeconds,0);
  assert.ok(!r.frameGeneration.enabled&&!r.fluid.validated&&!c.auditedFrames&&!m.auditedFrames&&!b.validated);
  assert.ok(c.timeCenteredPressure&&m.cutPressure);assert.equal(!!s,mode==='support');
  assert.equal(m.multigrid.exhaustedSolves+b.phaseLimiter.exhaustedSteps+b.invalid,0);
  if(s){assert.equal(s.projectionCalls,600);assert.equal(s.auditedFrames+s.auditedIdleFrames+s.invalid,0);}
  profiles.push({mode,repeat,rawMs:r.medianMs[5],fluidMs:r.medianMs[6],coverageMeanMs:s?.meanFrameMs??0,pressureMeanMs:m.meanMs});
}
const median=a=>a.toSorted((a,b)=>a-b)[a.length>>1],performance={};
if(!validationOnly)for(const mode of ['reference','support'])performance[mode]=Object.fromEntries(['rawMs','fluidMs','coverageMeanMs','pressureMeanMs'].map(key=>[key,median(profiles.filter(r=>r.mode===mode).map(r=>r[key]))]));
const hash=createHash('sha256');for(const n of names){hash.update(n);hash.update(readFileSync(join(shaders,n)));}
console.log(JSON.stringify({schema:1,date:new Date().toISOString(),baseCommit:'cee3689',gpu:'NVIDIA RTX 5090',nodeTests:167,windowsCTests:7,
  scope:'Opt-in filled Eulerian pressure/velocity support; particle-owned mass and free surface remain. Not complete Narrow Band FLIP.',
  defaultPromotionReady:false,profiled:!validationOnly,cases,comparisons,profiles,performance,
  caveats:['Coverage is audited on the last projection of each active frame; idle frames separately check zero coverage counters.',
    'Full-cell pressure support is not a geometric free-surface fraction or conservative flowing grid/particle ownership exchange.',
    'The phase donor cap and particle/bulk interface mismatch still permit closing-cell residual inventory.',
    'Mean support GPU time includes all frames, whereas raw/fluid medians exclude the initial 32-frame warmup.',
    'No new GBV-clean claim: prior Windows Graphics Tools initialization failed with 0x887A002D.'],
  exeSha256:createHash('sha256').update(readFileSync(exe)).digest('hex'),shaderSetSha256:hash.digest('hex')},null,2));
