// Run test-restir-pt.ps1 and test-fluid-traversal.ps1 (with/without -NoProbes).
import {readFileSync} from 'node:fs';
import {join} from 'node:path';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
import {readCapture} from './validate-captures.mjs';
const folder=process.argv[2];
if(!folder)throw new Error('Usage: node engine/validate-restir-pt.mjs runtime-directory');
const median=xs=>[...xs].sort((a,b)=>a-b)[Math.floor(xs.length/2)];
const cases=['reference','fresh','spatial','temporal','combined','moving','room-reference','room','orbit','nvapi','fixed','frame-gen'];
function opaqueMean(channels){
  const v=channels[0].values,a=channels[4].values;let count=0,sum=[0,0,0];
  for(let i=0;i<v.length;i+=4)if(a[i]+a[i+1]+a[i+2]>0){count++;for(let c=0;c<3;c++)sum[c]+=v[i+c];}
  return {count,meanRgb:sum.map(x=>x/count)};
}
const runs=cases.map(name=>{
  const {report:r,channels,summary}=readCapture(join(folder,`restir-${name}`));
  assert.equal(r.frames,r.dlssEvaluations);assert.equal(r.restirPT.invalidLastFrame,0);
  if(r.restirPT.enabled)assert.ok(r.restirPT.pixelsLastFrame>0);
  if(['combined','temporal','spatial'].includes(name))assert.ok(r.restirPT.reusedLastFrame>0);
  if(['combined','temporal'].includes(name))assert.ok(r.restirPT.temporalFrames>0);
  if(['moving','room','fresh','spatial'].includes(name))assert.equal(r.restirPT.temporalFrames,0);
  if(name==='frame-gen')assert.ok(r.frameGeneration.extraPresents>40&&r.frameGeneration.status===0);
  return {name,frames:r.frames,pt:r.restirPT,medianMs:r.medianMs,rawOpaque:opaqueMean(channels),capture:summary};
});
const reference=runs[0].rawOpaque.meanRgb,fresh=runs[1].rawOpaque.meanRgb;
for(const c of [0,1,2])assert.ok(Math.abs(fresh[c]/reference[c]-1)<.03,'Fresh PT/reference brightness mismatch');
for(const run of runs.slice(2,5))for(const c of [0,1,2])
  assert.ok(Math.abs(run.rawOpaque.meanRgb[c]/fresh[c]-1)<.03,'Reuse changed mean brightness');
const traversal=[];
for(const probes of [true,false])for(let repeat=0;repeat<3;repeat++)for(const mode of ['full','tight']){
  const name=`traversal-${probes?'':'raw-'}${mode}-${repeat}`;
  const {report:r,summary}=readCapture(join(folder,name));
  assert.equal(r.fluidTightTraversal,mode==='tight');assert.equal(r.fluidProbes.badRoots,0);
  assert.equal(r.fluidProbes.truncated,0);assert.equal(r.dlssEvaluations,240);
  if(probes)assert.ok(r.fluidProbes.hits>0&&r.fluid.validated);
  assert.equal(r.frameGeneration.extraPresents,0);assert.equal(r.restirPT.enabled,false);
  traversal.push({name,probes,mode,medianMs:r.medianMs,cells:r.fluidProbes.cellSteps,
    intersections:r.fluidProbes.intersections,roots:r.fluidProbes.hits,capture:summary});
}
const grouped=(probes,mode)=>traversal.filter(r=>r.probes===probes&&r.mode===mode);
const full=grouped(false,'full'),tight=grouped(false,'tight');
const rawChanges={};
for(const [name,c] of [['photons',0],['camera',2],['rawFrame',5],['simulation',6],['reconstruction',7]]){
  const before=median(full.map(r=>r.medianMs[c])),after=median(tight.map(r=>r.medianMs[c]));
  rawChanges[name]={fullMedianMs:before,tightMedianMs:after,reductionPercent:100*(1-after/before)};
}
const visitsBefore=median(grouped(true,'full').map(r=>r.cells)),visitsAfter=median(grouped(true,'tight').map(r=>r.cells));
// This tests less traversal work, not a fragile wall-clock threshold.
assert.ok(visitsAfter<visitsBefore*.8);
for(let i=0;i<3;i++){
  const a=grouped(true,'full')[i],b=grouped(true,'tight')[i];
  for(let c=0;c<3;c++)assert.ok(Math.abs(a.capture.powerXYZ[c]/b.capture.powerXYZ[c]-1)<.02,'Traversal changed integrated caustic energy');
}
const names=['Transport-0-1','Transport-1-1','Transport-2-1','FluidSurfaceBounds'];
console.log(JSON.stringify({context:'RTX 5090; 1280x720 Balanced; FG off except explicit FG case. 100k starting carriers, 120 Hz / two simulation steps per real frame. Three interleaved same-binary full/tight pairs. Per-cell probes OFF for raw timing claims. Room source is 140 W in BOTH A/B modes. Absolute timings exclude outer game update and are not universal FPS guarantees.',
  limitations:'PT scope is opaque-primary diffuse indirect, not refractive-primary GI or unified DI/PT. Single-frame mean-brightness checks are smoke tests, not an equal-time convergence/variance proof. GPU debug layer was unavailable; shaders/build, actual DXR probes, finite raw guide checks and DRED-compatible execution are distinct checks.',
  runs,traversal,rawChanges,cellVisitReductionPercent:100*(1-visitsAfter/visitsBefore),
  shaderSha256:Object.fromEntries(names.map(n=>[n,createHash('sha256').update(readFileSync(join(folder,'shaders',`${n}.dxil`))).digest('hex')]))},null,2));
