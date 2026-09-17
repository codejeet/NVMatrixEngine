// Read-only evidence collector. Save its JSON with apply_patch, never shell redirection.
import {readFileSync,readdirSync,statSync} from 'node:fs';
import {join} from 'node:path';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
import {readCapture} from './validate-captures.mjs';
const folder=process.argv[2];
if(!folder)throw new Error('Usage: node engine/record-density-kernel.mjs RUNTIME');
const exe=join(folder,'NVMatrixFluidLab.exe'),shaders=join(folder,'shaders');
const names=readdirSync(shaders).filter(n=>n.endsWith('.dxil')).sort();
const built=Math.max(statSync(exe).mtimeMs,...names.map(n=>statSync(join(shaders,n)).mtimeMs));
function read(name){const p=join(folder,name+'.json');assert.ok(statSync(p).mtimeMs>=built,`Stale ${name}`);return JSON.parse(readFileSync(p));}
const expected={empty:4,'room-short':8,calm:120,interior:120,cycle:120,fall:120,room:120,wake:180,controls:12,relaxation:12,adaptive:64};
const cases=Object.entries(expected).map(([name,frames])=>{
  const prefix=`density-kernel-cached-${name}`,r=read(prefix),m=r.fluid.adaptiveMac,k=r.fluid.cutCells;
  assert.equal(r.frames,frames);assert.ok(r.fluid.validated&&m.validated&&m.cutPressure&&k.validated&&k.solidKernelCache&&k.solidKernelEnabled);
  assert.equal(k.auditedFrames,frames);assert.equal(m.invalid+k.invalid+k.maxHistoryError,0);assert.ok(k.maxSolidKernelError<=.003);
  if(name!=='relaxation')assert.ok(m.multigrid.validated&&!m.multigrid.exhaustedSolves&&m.multigrid.peakFinalDivergence<=.000101);
  if(['calm','interior','cycle','wake'].includes(name))assert.ok(r.fluid.postStepMaxRelativeDensity<=1.05);
  const optical=readCapture(join(folder,prefix)).summary;
  assert.equal((r.fluidProbes?.badRoots??0)+(r.fluidProbes?.truncated??0),0);
  return {name,frames,steps:r.fluid.steps,coarseLeaves:m.coarseLeaves,
    kernelError:k.maxSolidKernelError,rebuilt:k.lastEndpointKernelRebuilt,reused:k.lastEndpointKernelReused,
    geometryError:k.maxGeometryError,matrixError:m.matrixError,fluxError:m.fluxError,
    peakDivergence:m.multigrid?.peakFinalDivergence??null,canonicalDivergence:m.canonicalDivergence,
    restMassUnits:r.fluid.restMassUnits,density:r.fluid.postStepMaxRelativeDensity,
    surfaceVolume:r.fluidSurface.tetrahedralVolumeEstimate,repairs:r.fluid.densityRepairsLastSubstep,optical};
});
const fixtures=['plane','oblique','moving-plane','sphere'].map(name=>{
  const r=read(`cut-cells-${name}`),k=r.fluid.cutCells;
  assert.ok(k.validated&&k.solidKernelEnabled&&k.maxSolidKernelError<=(name.includes('plane')?1e-5:.003));
  return {name,frames:r.frames,kernelError:k.maxSolidKernelError,updates:k.updates,
    rebuilt:k.lastEndpointKernelRebuilt,reused:k.lastEndpointKernelReused};
});
const physicalComparisons=['calm','room','adaptive'].map(name=>{
  const a=read(`density-kernel-legacy-${name}`),b=read(`density-kernel-cached-${name}`);
  assert.ok(a.fluid.validated&&a.fluid.adaptiveMac.validated&&!a.fluid.adaptiveMac.cutPressure&&!a.fluid.cutCells.solidKernelEnabled);
  assert.equal(a.frames,b.frames);assert.equal(a.fluid.restMassUnits,b.fluid.restMassUnits);
  const densityA=a.fluid.postStepMaxRelativeDensity,densityB=b.fluid.postStepMaxRelativeDensity;
  const volumeDifference=Math.abs(b.fluidSurface.tetrahedralVolumeEstimate/a.fluidSurface.tetrahedralVolumeEstimate-1);
  return {name,legacyDensity:densityA,newDensity:densityB,surfaceVolumeRelativeDifference:volumeDifference,
    densityGatePassed:densityB<=densityA+.01,volumeGatePassed:volumeDifference<=.01};
});
const profiles=[];
for(let repeat=1;repeat<=3;repeat++)for(const mode of ['legacy','full','cached']){
  const r=read(`density-kernel-cost-${mode}-${repeat}`),m=r.fluid.adaptiveMac,k=r.fluid.cutCells;
  assert.equal(r.frames,300);assert.equal(r.sampleCount,268);assert.equal(r.fluid.steps,600);
  assert.ok(!r.frameGeneration.enabled&&!r.fluid.validated&&!m.auditedFrames&&!k.auditedFrames&&!m.multigrid.exhaustedSolves);
  assert.equal(r.photonCounters[5]+r.photonCounters[6],0);
  for(const x of [r.medianMs[5],r.medianMs[6],m.meanMs,k.meanFrameMs])assert.ok(Number.isFinite(x)&&x>0);
  assert.equal(m.cutPressure,mode!=='legacy');assert.equal(k.solidKernelEnabled,mode!=='legacy');
  if(mode!=='legacy')assert.equal(k.solidKernelCache,mode==='cached');
  profiles.push({mode,repeat,rawMs:r.medianMs[5],fluidMs:r.medianMs[6],pressureMeanMs:m.meanMs,
    geometryMeanMs:k.meanFrameMs,rebuilt:k.lastEndpointKernelRebuilt,reused:k.lastEndpointKernelReused});
}
const median=a=>a.toSorted((x,y)=>x-y)[a.length>>1],performance={};
for(const mode of ['legacy','full','cached']){
  const p=profiles.filter(p=>p.mode===mode);
  performance[mode]=Object.fromEntries(['rawMs','fluidMs','pressureMeanMs','geometryMeanMs'].map(key=>[key,median(p.map(r=>r[key]))]));
}
const hash=createHash('sha256');for(const n of names){hash.update(n);hash.update(readFileSync(join(shaders,n)));}
console.log(JSON.stringify({schema:1,date:new Date().toISOString(),baseCommit:'6919305',gpu:'NVIDIA RTX 5090',
  scope:'Boundary-aware B-spline solid support and local recomputation. Not full adaptive-fluid completion or general conforming particle transport.',
  defaultPromotionReady:false,nodeTests:133,windowsCTests:7,cases,fixtures,physicalComparisons,profiles,performance,
  gpuValidation:{attempted:true,available:false,initializationError:'0x887A002D'},
  caveats:['Room density acceptance remains separate from kernel and pressure accuracy.',
    'Six/eight-point quadrature agreement is sampled, not a rigorous global error bound.',
    'Sparse physical allocation and authoritative flowing coarse bulk remain unimplemented.',
    'GPU validation was retried, but the Windows debug component is still unavailable (0x887A002D). No GBV-clean claim.'],
  exeSha256:createHash('sha256').update(readFileSync(exe)).digest('hex'),shaderSetSha256:hash.digest('hex')},null,2));
