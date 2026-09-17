// profile-room-paired.ps1 compares saved ORIGINAL DXIL with the current build.
// test-render-performance.ps1 supplies deterministic image/backend regressions.
import {readFileSync} from 'node:fs';
import {join} from 'node:path';
import assert from 'node:assert/strict';
import {readCapture} from './validate-captures.mjs';

const [folder,tag='visibility',mode]=process.argv.slice(2);
if(!folder)throw new Error('Usage: node engine/validate-room-performance.mjs runtime-directory tag [--profiles-only|--parity-only]');
const read=name=>JSON.parse(readFileSync(join(folder,`${name}.json`),'utf8'));
const quantile=(xs,q)=>[...xs].sort((a,b)=>a-b)[Math.floor(q*(xs.length-1))];
const manifest=read(`room-perf-${tag}-manifest`);
assert.equal(manifest.runs.length%6,0);assert.ok(manifest.runs.length>=18);
assert.equal(manifest.shaderHashes.length,6);
assert.ok(manifest.shaderHashes.every(s=>s.baseline!==s.candidate),'Shader A/B used identical libraries');
function profiles(version,view){
  const names=manifest.runs.filter(n=>n.includes(`-${version}-${view}-`)),reports=names.map(read);
  assert.ok(names.length>=3);
  for(const [i,r] of reports.entries()){
    assert.equal(r.frames,300);assert.equal(r.sampleCount,268);assert.equal(r.warmupFrames,32);
    assert.equal(r.outputWidth,1920);assert.equal(r.outputHeight,1080);assert.equal(r.dlssMode,'balanced');
    assert.equal(r.photonsPerFrame,65536);assert.equal(r.hitMode,0);assert.equal(r.floatAtomics,true);
    assert.equal(r.restirPT.enabled,false);assert.equal(r.fluidTightTraversal,true);
    assert.equal(r.fluid.initialParticles,100000);assert.equal(r.fluid.capacity,250000);
    assert.equal(r.fluid.steps,600);assert.equal(r.fluid.droppedSeconds,0);assert.equal(r.fluid.validated,false);
    assert.equal(r.fluid.roomPool,true);assert.equal(r.fluid.emittedParticles,5899);
    assert.equal(r.fluidSurface.anisotropic,true);assert.equal(r.whitewater.capacity,8192);assert.equal(r.whitewater.invalid,0);
    assert.equal(r.frameGeneration.enabled,false);assert.equal(r.dlssEvaluations,300);assert.equal(r.rrHistoryResets,1);
    assert.equal(r.photonCounters[5],0);assert.equal(r.photonCounters[6],0);
    assert.equal(r.orbitTest,view==='orbit');assert.equal(r.rollingTest,view==='rolling');
    for(const row of r.samplesMs){assert.equal(row.length,r.timingColumns.length);assert.ok(row.every(x=>Number.isFinite(x)&&x>=0));}
    readCapture(join(folder,names[i])); // actual finite raw/RR/guide/atlas data, not just timings
  }
  const rows=reports.flatMap(r=>r.samplesMs),columns=reports[0].timingColumns;
  const medianMs=Object.fromEntries(columns.map((name,i)=>[name,quantile(rows.map(r=>r[i]),.5)]));
  return {names,samples:rows.length,medianMs,frameP95Ms:quantile(rows.map(r=>r[5]),.95),
    reciprocalMedianRenderFps:1000/medianMs.frame,
    perRun:reports.map(r=>({medianMs:r.medianMs,frameP95Ms:quantile(r.samplesMs.map(x=>x[5]),.95)}))};
}
function parity(kind,backend){
  const a=readCapture(join(folder,`render-parity-before-${kind}-${backend}`));
  const b=readCapture(join(folder,`render-parity-after-${kind}-${backend}`));
  assert.equal(a.report.transportHash,b.report.transportHash);
  assert.equal(a.report.frames,b.report.frames);assert.equal(a.report.hitMode,b.report.hitMode);
  assert.equal(a.report.floatAtomics,b.report.floatAtomics);
  const channels=a.channels.map((channel,i)=>{
    const x=channel.values,y=b.channels[i].values;assert.equal(x.length,y.length);
    let error=0,magnitude=0,max=0;
    for(let j=0;j<x.length;j++){const d=Math.abs(x[j]-y[j]);error+=d;magnitude+=Math.abs(x[j]);max=Math.max(max,d);}
    return {index:i,relativeL1:error/Math.max(magnitude,1e-30),maxAbs:max};
  });
  // Raw radiance, geometry/material guides and irradiance agree to 0.001%;
  // learned RR output to 0.5%. Motion uses a near-zero-denominator guard.
  for(const c of channels)assert.ok(c.relativeL1<(c.index===7?.005:c.index===1?.001:.00001),`${kind}/${backend} channel ${c.index}: ${c.relativeL1}`);
  assert.deepEqual(a.report.cameraPathTotals,b.report.cameraPathTotals,'Camera event totals changed');
  assert.equal(a.report.cameraTruncatedLastFrame,b.report.cameraTruncatedLastFrame);
  if(kind!=='play'){
    assert.equal(a.report.fluidProbes.hits,b.report.fluidProbes.hits);
    assert.equal(b.report.fluidProbes.glassShellHits,10);
    assert.equal(b.report.fluidProbes.badRoots,0);assert.equal(b.report.fluidProbes.truncated,0);
  }
  return {kind,backend,channels,cameraPathTotals:b.report.cameraPathTotals,
    cameraTruncatedLastFrame:{before:a.report.cameraTruncatedLastFrame,after:b.report.cameraTruncatedLastFrame},probes:b.report.fluidProbes};
}
const comparisons=mode==='--parity-only'?undefined:['play','orbit','rolling'].map(view=>{
  const before=profiles('before',view),after=profiles('after',view);
  return {view,before,after,cameraTimeReductionPercent:100*(1-after.medianMs.camera/before.medianMs.camera),
    frameTimeReductionPercent:100*(1-after.medianMs.frame/before.medianMs.frame),
    renderFpsGainPercent:100*(before.medianMs.frame/after.medianMs.frame-1)};
});
console.log(JSON.stringify({
  context:'RTX 5090, 1080p Balanced (1114x626 internal), live room inlet/foam/bubbles, 100k initial / 250k cap, 120 Hz APIC, two substeps per real frame, unchanged 120 pressure / 60 density iterations, 65,536 photons, FG off, no probes. Three interleaved A/B pairs per view; pair order reverses on repeat 2. First 32 frames excluded. Renderer frame interval includes submission/present/fence but not outer gameplay update; reciprocal medians are not displayed-FG FPS. Column medians need not sum. Live simulation summation order can vary between runs.',
  manifest,comparisons,
  parity:mode==='--profiles-only'?undefined:['sphere','sheet','play'].flatMap(k=>['plain','dxr','nvapi','fixed'].map(b=>parity(k,b)))
},null,2));
