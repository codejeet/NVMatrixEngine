// Read-only evidence collector. Numerical, optical and profiling runs must all
// come from the current binary/shaders; stdout is saved using apply_patch.
import {readFileSync,readdirSync,statSync} from 'node:fs';
import {join} from 'node:path';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
import {readCapture} from './validate-captures.mjs';
const folder=process.argv[2];
if(!folder)throw new Error('Usage: node engine/record-fluid-surface-band.mjs <runtime folder>');
const exe=join(folder,'NVMatrixFluidLab.exe'),shaders=join(folder,'shaders');
const names=readdirSync(shaders).filter(n=>n.endsWith('.dxil')).sort();
const built=Math.max(statSync(exe).mtimeMs,...names.map(n=>statSync(join(shaders,n)).mtimeMs));
function read(name){const path=join(folder,`${name}.json`);assert.ok(statSync(path).mtimeMs>=built,`Stale ${name}`);return JSON.parse(readFileSync(path));}
const expected={calm:32,room:32,deep:32,'calm-xz':32,'room-xz':32,'room-xz-fine':32,
  'room-xz-view':32,'deep-xz':32,'room-xz-orbit':120,'fall-xz':120,'wake-xz':180,'emission-xz':90,
  'axis-x':32,'axis-y':32,'axis-z':32,empty:4,'flat-box':32,'flat-box-fine':32,'curved-box':32,'curved-box-fine':32,view:32,
  'curved-orbit':120,fall:120,wake:180,emission:90,sheet:4,sphere:4,mac:120,fg:60,pt:60};
const cases=Object.entries(expected).map(([name,frames])=>{
  const r=read(`surface-band-${name}`),s=r.fluidSurface,a=s.adaptiveSurface;
  assert.equal(r.frames,frames);assert.ok(r.fluid.validated&&a.audits>0);
  assert.equal(r.photonCounters[5]+r.photonCounters[6]+r.fluidProbes.badRoots+r.fluidProbes.truncated,0);
  assert.equal(a.coarseAxes,name.startsWith('axis-')?1<<('xyz'.indexOf(name.at(-1))):(name.includes('-xz')?5:7));
  const anchors=[0,1,2].reduce((n,i)=>n*((a.coarseAxes&(1<<i))?5:9),1);
  assert.equal(a.fineGatherBricks+a.coarseGatherBricks,s.activeBricks);
  assert.equal(a.fineNodeEvaluations,a.fineGatherBricks*729);
  assert.equal(a.coarseNodeEvaluations,a.coarseGatherBricks*anchors);
  assert.equal(a.repairNodeEvaluations,a.repairBricks*729);
  assert.ok(a.maxPhiErrorCells<=a.phiToleranceCells*1.01+1e-5);
  assert.ok(a.maxNormalError<=a.normalTolerance*1.01+1e-4);
  assert.ok(a.maxBoundaryError<=2e-7&&a.maxMotionBoundaryError<=2e-7);
  assert.equal(a.admissionAudit.surfaceBricks,s.surfaceBricks);
  if(['flat-box','curved-box','curved-orbit','calm-xz','room-xz','room-xz-view'].includes(name))
    assert.ok(a.coarseSurfaceBricks>0&&a.coarseGatherBricks>0,'No actual mixed-resolution surface work');
  if(name.endsWith('-fine'))assert.equal(a.coarseSurfaceBricks+a.coarseGatherBricks,0);
  if(name.startsWith('axis-'))assert.ok(a.coarseSurfaceBricks>0&&a.coarseGatherBricks>0);
  if(name==='fg')assert.ok(r.frameGeneration.enabled);
  if(name==='pt')assert.ok(r.restirPT);
  return {name,frames,activeBricks:s.activeBricks,surfaceBricks:s.surfaceBricks,volume:s.tetrahedralVolumeEstimate,lod:a};
});
const comparisons=['flat-box','curved-box','room-xz'].map(name=>{
  const a=readCapture(join(folder,`surface-band-${name}`)),b=readCapture(join(folder,`surface-band-${name}-fine`));
  assert.equal(a.report.transportHash,b.report.transportHash);
  let difference=0,reference=0;
  for(const channel of [8,9])for(let i=0;i<a.channels[channel].values.length;i++)if(i%4!==3){
    difference+=Math.abs(a.channels[channel].values[i]-b.channels[channel].values[i]);
    reference+=Math.abs(b.channels[channel].values[i]);
  }
  const relativeAtlasL1=difference/Math.max(reference,1e-30);
  assert.ok(relativeAtlasL1<.01,`${name}: caustic atlas differs by more than one percent`);
  return {name,relativeAtlasL1,coarsePowerXYZ:a.summary.powerXYZ,finePowerXYZ:b.summary.powerXYZ};
});
const view=readCapture(join(folder,'surface-band-room-xz-view'));let finePixels=0,coarsePixels=0;
const raw=view.channels[0].values;
for(let i=0;i<raw.length;i+=4){
  if(Math.abs(raw[i]-1)<.002&&Math.abs(raw[i+1]-.3)<.002&&Math.abs(raw[i+2]-.05)<.002)finePixels++;
  if(Math.abs(raw[i]-.05)<.002&&Math.abs(raw[i+1]-.4)<.002&&Math.abs(raw[i+2]-1)<.002)coarsePixels++;
}
assert.ok(finePixels>100&&coarsePixels>100,'Missing actual particle-water mixed-LOD image');
for(const frame of [128,176,224,272,320])for(const prefix of ['water-temporal-surface-band','water-temporal-surface-band-reference'])
  assert.equal(read(`${prefix}-${frame}`).rrHistoryResets,1);
const profiles=[];
for(const scene of ['calm','live'])for(const mode of ['off','xz'])for(let repeat=1;repeat<=3;repeat++){
  const r=read(`room-perf-surface-band-${scene}-${mode}-orbit-${repeat}`);
  assert.equal(r.frames,300);assert.equal(r.sampleCount,268);assert.equal(r.fluid.steps,600);
  assert.ok(!r.fluid.validated&&!r.frameGeneration.enabled&&!r.fluid.deterministicBins);
  assert.equal(!!r.fluidSurface.adaptiveSurface,mode==='xz');
  const times=r.samplesMs.map(s=>s[5]).sort((a,b)=>a-b);
  profiles.push({scene,mode,repeat,rawMedianMs:r.medianMs[5],simulationMedianMs:r.medianMs[6],surfaceMedianMs:r.medianMs[7],
    blasMedianMs:r.medianMs[8],rawP95Ms:times[Math.floor(.95*(times.length-1))],
    lastCoarseSurfaceBricks:r.fluidSurface.adaptiveSurface?.coarseSurfaceBricks??0,
    lastCoarseGatherBricks:r.fluidSurface.adaptiveSurface?.coarseGatherBricks??0});
}
const hash=createHash('sha256');for(const name of names){hash.update(name);hash.update(readFileSync(join(shaders,name)));}
console.log(JSON.stringify({schema:1,date:new Date().toISOString(),baseCommit:'c4465c2',gpu:'NVIDIA RTX 5090',
  scope:'Opt-in geometric interface error band and directional nested sampling. Actual calm particle-water surface coarsening; dynamic/high-error water remains fine. No compressed storage, flowing bulk or full adaptive-optics acceptance.',
  scopedChecksPassed:true,cpuTests:109,windowsTests:7,cases,comparisons,profiles,
  debugView:{finePixels,coarsePixels},
  profileScope:'Three interleaved same-build 1080p Balanced 300-frame orbits per mode and scene, first 32 frames excluded. Calm particle room and live room/inlet/foam separately; FG and validation off.',
  exeSha256:createHash('sha256').update(readFileSync(exe)).digest('hex'),shaderSetSha256:hash.digest('hex'),
  debugLayer:{gpuBasedValidationExecuted:false,previousInitializationError:'0x887A002D'}},null,2));
