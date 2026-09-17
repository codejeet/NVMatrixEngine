// Read-only scoped evidence; save output with apply_patch. No GPU launches.
import {readFileSync,readdirSync,statSync} from 'node:fs';
import {join} from 'node:path';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
import {readCapture} from './validate-captures.mjs';
const folder=process.argv[2];
if(!folder)throw new Error('Usage: node engine/record-cut-pressure.mjs runtime-folder');
const exe=join(folder,'NVMatrixFluidLab.exe'),shaders=join(folder,'shaders');
const names=readdirSync(shaders).filter(n=>n.endsWith('.dxil')).sort();
const built=Math.max(statSync(exe).mtimeMs,...names.map(n=>statSync(join(shaders,n)).mtimeMs));
function read(name){const p=join(folder,name+'.json');assert.ok(statSync(p).mtimeMs>=built,`Stale ${name}`);return JSON.parse(readFileSync(p));}
const expected={empty:4,'room-short':8,calm:120,interior:120,cycle:120,fall:120,room:120,wake:180,controls:12,relaxation:12,adaptive:64};
const cases=Object.entries(expected).map(([name,frames])=>{
  const r=read(`cut-pressure-${name}`),m=r.fluid.adaptiveMac,c=r.fluid.cutCells;
  assert.equal(r.frames,frames);assert.ok(r.fluid.validated&&m.validated&&m.cutPressure&&c.validated);
  assert.equal(c.auditedFrames,frames);assert.equal(m.invalid+c.invalid+c.maxHistoryError,0);
  assert.equal(r.photonCounters[5]+r.photonCounters[6]+(r.fluidProbes?.badRoots??0)+(r.fluidProbes?.truncated??0),0);
  if(name!=='relaxation'){
    assert.ok(m.multigrid.validated);assert.equal(m.multigrid.exhaustedSolves,0);
    assert.ok(m.multigrid.peakFinalDivergence<=.000101&&m.preciseMatrixError<=1e-10);
  }
  if(['calm','interior','cycle','wake'].includes(name))assert.ok(r.fluid.postStepMaxRelativeDensity<=1.05);
  const optical=readCapture(join(folder,`cut-pressure-${name}`)).summary;
  return {name,frames,steps:r.fluid.steps,mac:m,cutGeometry:c,optical,
    particle:{restMassUnits:r.fluid.restMassUnits,postStepMaxRelativeDensity:r.fluid.postStepMaxRelativeDensity,
      postStepOccupiedVolume:r.fluid.postStepOccupiedVolume,surfaceVolume:r.fluidSurface.tetrahedralVolumeEstimate,
      repairLimit:r.fluid.densityRepairLimit,repairsLastSubstep:r.fluid.densityRepairsLastSubstep,maxCfl:r.fluid.maxParticleCfl}};
});
const physicalComparisons=['room','adaptive'].map(name=>{
  const a=read(`cut-pressure-legacy-${name}`),b=read(`cut-pressure-${name}`);
  assert.ok(a.fluid.validated&&a.fluid.adaptiveMac.validated&&!a.fluid.adaptiveMac.cutPressure);
  assert.equal(a.frames,b.frames);assert.equal(a.fluid.restMassUnits,b.fluid.restMassUnits);
  const densityA=a.fluid.postStepMaxRelativeDensity,densityB=b.fluid.postStepMaxRelativeDensity;
  const volumeDifference=Math.abs(b.fluidSurface.tetrahedralVolumeEstimate/a.fluidSurface.tetrahedralVolumeEstimate-1);
  return {name,legacyPeakDensity:densityA,cutPeakDensity:densityB,surfaceVolumeRelativeDifference:volumeDifference,
    legacyDensityGatePassed:densityB<=densityA+.01,legacyVolumeGatePassed:volumeDifference<=.01};
});
const profiles=[];
for(let repeat=1;repeat<=3;repeat++)for(const mode of ['legacy','cut']){
  const r=read(`cut-pressure-cost-${mode}-${repeat}`),m=r.fluid.adaptiveMac;
  assert.equal(r.frames,300);assert.equal(r.sampleCount,268);assert.equal(r.fluid.steps,600);
  assert.ok(!r.frameGeneration.enabled&&!r.fluid.validated&&!m.auditedFrames&&!r.fluid.cutCells.auditedFrames);
  assert.equal(m.cutPressure,mode==='cut');assert.equal(m.multigrid.exhaustedSolves,0);
  assert.equal(r.photonCounters[5]+r.photonCounters[6],0);
  profiles.push({mode,repeat,rawMs:r.medianMs[5],fluidMs:r.medianMs[6],pressureMeanMs:m.meanMs});
}
const median=a=>a.toSorted((x,y)=>x-y)[a.length>>1],performance={};
for(const mode of ['legacy','cut']){
  const r=profiles.filter(p=>p.mode===mode);
  performance[mode]={rawMs:median(r.map(p=>p.rawMs)),fluidMs:median(r.map(p=>p.fluidMs)),pressureMeanMs:median(r.map(p=>p.pressureMeanMs))};
}
const hash=createHash('sha256');for(const n of names){hash.update(n);hash.update(readFileSync(join(shaders,n)));}
console.log(JSON.stringify({schema:1,date:new Date().toISOString(),baseCommit:'22f79a5',gpu:'NVIDIA RTX 5090',
  scope:'Opt-in cut-cell pressure/flux contract. Not full adaptive fluid delivery, flowing narrow-band ownership or a physical-conservation proof.',
  defaultPromotionReady:false,nodeTests:127,windowsCTests:7,cases,physicalComparisons,profiles,performance,
  caveats:['Pressure and geometric audits are not particle-density acceptance.',
    'Liquid remains particle-owned; no authoritative VOF/coarse bulk transport.',
    'Precise shared-face flux is separate from the rounded APIC transfer velocity.',
    'Sampled solid geometry can miss thin/subgrid features; fully closing cells need further conservative treatment.',
    'GPU-based validation initialization failed again: Windows debug component unavailable (0x887A002D).'],
  exeSha256:createHash('sha256').update(readFileSync(exe)).digest('hex'),shaderSetSha256:hash.digest('hex')},null,2));
