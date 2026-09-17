// Read-only fresh-build evidence collector. Persist output using apply_patch.
import {readFileSync,readdirSync,statSync} from 'node:fs';
import {join} from 'node:path';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
const folder=process.argv[2];
if(!folder)throw new Error('Usage: node engine/record-phase-flux.mjs RUNTIME');
const exe=join(folder,'NVMatrixFluidLab.exe'),shaders=join(folder,'shaders');
const names=readdirSync(shaders).filter(n=>n.endsWith('.dxil')).sort();
const built=Math.max(statSync(exe).mtimeMs,...names.map(n=>statSync(join(shaders,n)).mtimeMs));
function read(name){const p=join(folder,name+'.json');assert.ok(statSync(p).mtimeMs>=built,`Stale ${name}`);return JSON.parse(readFileSync(p));}
const expected={'initial-calm':1,'initial-room':1,empty:4,short:8,calm:120,fall:90,room:120,wake:180,controls:12,reset:180,adaptive:64,overlay:32};
const cases=Object.entries(expected).map(([name,frames])=>{
  const r=read(`bulk-bounded-${name}`),b=r.fluid.bulk,l=b.phaseLimiter,a=b.sourceAllocation,m=r.fluid.adaptiveMac;
  assert.equal(r.frames,frames);assert.equal(l.steps,b.steps);assert.equal(b.steps,r.fluid.steps);
  assert.ok(b.validated&&a.validated&&m.validated&&r.fluid.cutCells.validated&&r.fluid.validated);
  assert.ok(l.steps===0||l.validated);assert.equal(b.invalid+l.invalid+l.exhaustedSteps+a.invalid+m.invalid+r.fluid.cutCells.invalid,0);
  assert.ok(b.relativeVolumeError<.0002&&l.peakNewExcessM3<1e-7&&a.newExcessM3<=1e-7);
  assert.ok(m.multigrid.validated&&!m.multigrid.exhaustedSolves&&m.multigrid.peakFinalDivergence<.000101);
  assert.equal(r.photonCounters[5]+r.photonCounters[6]+r.fluidProbes.badRoots+r.fluidProbes.truncated,0);
  if(['calm','fall'].includes(name))assert.ok(b.excessVolumeM3<1e-6);
  if(name==='controls')assert.ok(!a.advanced&&l.steps===0&&l.lastIterations===0&&l.lastFrameMs===0);
  if(name==='room')assert.ok(l.iterations>0&&b.excessVolumeM3<.01);
  return {name,frames,steps:b.steps,totalVolumeM3:b.volumeM3,pendingM3:a.pendingVolumeM3,
    totalMassError:b.relativeVolumeError,postTransportExcessM3:b.excessVolumeM3,
    closedResidentM3:r.fluid.cutCells.bulkInClosedCellsM3,limiter:l,peakDivergence:m.multigrid.peakFinalDivergence};
});
const comparisons=['initial-calm','initial-room','calm','room','wake','adaptive'].map(name=>{
  const a=read(`bulk-capacity-${name}`),b=read(`bulk-bounded-${name}`);
  assert.ok(!a.fluid.bulk.phaseLimiter&&a.fluid.bulk.validated);assert.equal(a.fluid.restMassUnits,b.fluid.restMassUnits);
  return {name,beforeResidentExcessM3:a.fluid.bulk.excessVolumeM3,afterResidentExcessM3:b.fluid.bulk.excessVolumeM3,
    afterClosedResidentM3:b.fluid.cutCells.bulkInClosedCellsM3};
});
const profiles=[];
for(let repeat=1;repeat<=3;repeat++)for(const mode of ['off','on']){
  const r=read(`phase-flux-cost-${mode}-${repeat}`),b=r.fluid.bulk,l=b.phaseLimiter,m=r.fluid.adaptiveMac;
  assert.equal(r.frames,300);assert.equal(r.sampleCount,268);assert.equal(r.fluid.steps,600);
  assert.ok(!r.frameGeneration.enabled&&!r.fluid.validated&&!b.validated&&!m.auditedFrames&&!m.multigrid.exhaustedSolves);
  assert.equal(r.photonCounters[5]+r.photonCounters[6]+b.invalid,0);
  assert.ok(b.sourceAllocation&&!b.sourceAllocation.invalid);
  if(mode==='on')assert.ok(l&&!l.validated&&!l.invalid&&!l.exhaustedSteps&&l.steps===600);else assert.ok(!l);
  profiles.push({mode,repeat,rawMs:r.medianMs[5],fluidMs:r.medianMs[6],bulkMeanMs:b.meanFrameMs,
    limiterMeanMs:l?.meanFrameMs??0,limiterGpuBytes:l?.allocatedGpuBufferBytes??0});
}
const median=a=>a.toSorted((a,b)=>a-b)[a.length>>1],performance={};
for(const mode of ['off','on']){
  const p=profiles.filter(p=>p.mode===mode);
  performance[mode]=Object.fromEntries(['rawMs','fluidMs','bulkMeanMs','limiterMeanMs'].map(key=>[key,median(p.map(r=>r[key]))]));
}
const hash=createHash('sha256');for(const n of names){hash.update(n);hash.update(readFileSync(join(shaders,n)));}
console.log(JSON.stringify({schema:1,date:new Date().toISOString(),baseCommit:'f2d4a7a',gpu:'NVIDIA RTX 5090',nodeTests:154,windowsCTests:7,
  scope:'Receiver-limited shared phase transfers; particle-owned replica, not moving-cut closure or flowing Narrow Band FLIP.',
  defaultPromotionReady:false,cases,comparisons,profiles,performance,
  caveats:['Bounds are relative to max(current open capacity, initial resident inventory); preexisting solid-closure excess is retained and reported.',
    'GPU audits cover every substep; FP64 independent replay covers each captured frame final substep.',
    '128-iteration convergence is tested for these scenes, not proven for arbitrary graphs. Exhaustion fails, rather than silently accepting an unbounded step.',
    'FP32 volume tolerance is explicit; no stored-volume clamps or mass deletion are used.',
    'Compatible free-surface pressure support, moving-solid escape paths and conservative particle/grid handoff remain incomplete.',
    'No new GBV-clean claim: prior Windows Graphics Tools initialization failed with 0x887A002D.',
    'Exact optical isolation is checked separately with deterministic bins and fixed-point photons; not a new full temporal-sequence audit.'],
  exeSha256:createHash('sha256').update(readFileSync(exe)).digest('hex'),shaderSetSha256:hash.digest('hex')},null,2));
