// Read-only evidence collector: stdout is committed only after all checks pass.
import {readFileSync,readdirSync,statSync} from 'node:fs';
import {join} from 'node:path';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
const folder=process.argv[2];if(!folder)throw new Error('Usage: node engine/record-fluid-work.mjs <runtime folder>');
const exe=join(folder,'NVMatrixFluidLab.exe'),shaders=join(folder,'shaders');
const names=readdirSync(shaders).filter(n=>n.endsWith('.dxil')).sort();
const built=Math.max(statSync(exe).mtimeMs,...names.map(n=>statSync(join(shaders,n)).mtimeMs));
function read(name){const path=join(folder,`${name}.json`);assert.ok(statSync(path).mtimeMs>=built,`Stale ${name}`);return JSON.parse(readFileSync(path));}
const expectedFrames={empty:4,constant:1,affine:1,compression:1,roundtrip:1,controls:12,calm:120,fall:120,
  wake:180,interior:120,'interior-wake':180,'resample-fall':120,long:1200,room:90,odd:60,mac:120,mgpcg:120,fg:60,pt:60};
const cases=['empty','constant','affine','compression','roundtrip','controls','calm','fall','wake','interior',
  'interior-wake','resample-fall','long','room','odd','mac','mgpcg','fg','pt'].map(name=>{
  const a=read(`work-${name}-sparse`),b=read(`work-${name}-dense`),f=a.fluid,w=f.work;
  assert.equal(a.frames,expectedFrames[name]);assert.equal(b.frames,a.frames);
  assert.ok(f.validated&&w.validated&&b.fluid.validated&&!b.fluid.work);
  assert.equal(w.sameStateFaceMaxDifference,0);assert.equal(w.sameStateDensityMaxDifference,0);
  assert.ok(w.sameStateFaceComparisons>0);
  if(!['constant','affine','compression','roundtrip'].includes(name))assert.ok(w.sameStateDensityComparisons>0);
  assert.equal(f.active,b.fluid.active);assert.equal(f.particleMassUnits,b.fluid.particleMassUnits);
  assert.equal(f.densityIterations,b.fluid.densityIterations);
  const differences={};
  for(const key of ['postStepMaxRelativeDensity','postStepOccupiedVolume','meanHeight','maxSpeed'])if(f[key]!==undefined){
    differences[key]=f[key]-b.fluid[key];assert.ok(Math.abs(differences[key])<=2e-5*(1+Math.abs(b.fluid[key])));
  }
  let volumeDifference=0;
  if(a.fluidSurface){
    volumeDifference=a.fluidSurface.tetrahedralVolumeEstimate-b.fluidSurface.tetrahedralVolumeEstimate;
    assert.ok(Math.abs(volumeDifference)<=2e-5*(1+b.fluidSurface.tetrahedralVolumeEstimate));
    assert.equal(a.fluidProbes.badRoots+a.fluidProbes.truncated+a.photonCounters[5]+a.photonCounters[6],0);
  }
  if(f.adaptiveMac?.multigrid)assert.equal(f.adaptiveMac.multigrid.exhaustedSolves,0);
  if(name==='empty')assert.equal(w.lastFaceTiles+w.lastDensityTiles,0);
  if(name==='fg')assert.ok(a.frameGeneration.enabled);
  if(name==='pt')assert.ok(a.restirPT);
  return {name,frames:a.frames,steps:f.steps,samples:f.active,mass:f.particleMassUnits,volumeDifference,differences,work:w};
});
const profiles=[];
for(const mode of ['dense','sparse'])for(let repeat=1;repeat<=3;repeat++){
  const r=read(`room-perf-work-${mode}-orbit-${repeat}`);
  assert.equal(r.frames,300);assert.equal(r.sampleCount,268);assert.ok(!r.fluid.validated&&!r.frameGeneration.enabled&&!r.fluid.deterministicBins);
  const times=r.samplesMs.map(s=>s[5]).sort((a,b)=>a-b);
  profiles.push({mode,repeat,rawMedianMs:r.medianMs[5],fluidMedianMs:r.medianMs[6],rawP95Ms:times[Math.floor(.95*(times.length-1))],work:r.fluid.work??null});
}
const multigridProfiles=['dense','sparse'].map(mode=>{
  const r=read(`room-perf-work-mgpcg-${mode}-orbit-1`);
  assert.equal(r.frames,300);assert.ok(!r.fluid.validated&&!r.frameGeneration.enabled);
  assert.equal(r.fluid.adaptiveMac.multigrid.exhaustedSolves,0);
  return {mode,rawMedianMs:r.medianMs[5],fluidMedianMs:r.medianMs[6],work:r.fluid.work??null};
});
const view=read('work-view');assert.equal(view.frames,12);assert.ok(view.fluid.work.validated);
assert.ok(view.fluid.debugVisible);assert.equal(view.fluid.debugMode,7);
const viewImage=readFileSync(join(folder,'work-view.ppm'));
const viewHeader=/^P6\s+(\d+)\s+(\d+)\s+255\n/.exec(viewImage.subarray(0,64).toString());
assert.ok(viewHeader);let cyanPixels=0;
for(let i=viewHeader[0].length;i<viewImage.length;i+=3)
  if(viewImage[i]<5&&viewImage[i+1]>200&&viewImage[i+2]>245)cyanPixels++;
assert.ok(cyanPixels>10000,'Work overlay did not reach the captured image');
const temporal=[128,176,224,272,320].map(frame=>{
  const a=read(`water-temporal-work-${frame}`),b=read(`water-temporal-work-reference-${frame}`);
  assert.equal(a.rrHistoryResets,1);assert.equal(b.rrHistoryResets,1);
  return {frame,rrResets:a.rrHistoryResets};
});
const shaderHash=createHash('sha256');for(const name of names){shaderHash.update(name);shaderHash.update(readFileSync(join(shaders,name)));}
console.log(JSON.stringify({schema:1,date:new Date().toISOString(),baseCommit:'c1841a9',gpu:'NVIDIA RTX 5090',
  scope:'GPU-compacted exact P2G/density execution; dense transfer storage and broader adaptive specification remain incomplete',
  scopedChecksPassed:true,cpuTests:93,windowsTests:7,cases,profiles,multigridProfiles,temporal,
  debugView:{frames:view.frames,validated:view.fluid.work.validated,visible:view.fluid.debugVisible,mode:view.fluid.debugMode,cyanPixels},
  profileScope:'Three interleaved same-build 1080p Balanced room/inlet/foam/orbit runs per uniform/compact mode, plus one MGPCG comparison; FG and validation off.',
  exeSha256:createHash('sha256').update(readFileSync(exe)).digest('hex'),shaderSetSha256:shaderHash.digest('hex'),
  debugLayer:{gpuBasedValidationExecuted:false,previousInitializationError:'0x887A002D'}},null,2));
