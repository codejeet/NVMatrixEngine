// Run profile-fluid.ps1 with baseline/optimized tags before comparing. Baseline
// must be the saved pre-optimization executable AND shaders, not current code.
import {readFileSync} from 'node:fs';
import {join} from 'node:path';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
import {readCapture} from './validate-captures.mjs';

const folder=process.argv[2];
if(!folder)throw new Error('Usage: node engine/validate-fluid-performance.mjs runtime-directory');
const read=name=>JSON.parse(readFileSync(join(folder,`${name}.json`),'utf8'));
const quantile=(xs,q)=>[...xs].sort((a,b)=>a-b)[Math.floor(q*(xs.length-1))];
function profiles(tag,view){
  const names=[1,2].map(i=>`fluid-perf-${tag}-${view}-${i}`),reports=names.map(read);
  for(const r of reports){
    assert.equal(r.frames,360);assert.equal(r.sampleCount,328);assert.equal(r.warmupFrames,32);
    assert.equal(r.outputWidth,1920);assert.equal(r.outputHeight,1080);assert.equal(r.dlssMode,'balanced');
    assert.equal(r.photonsPerFrame,65536);assert.equal(r.fluid.particles,100000);assert.equal(r.fluid.steps,720);
    assert.equal(r.fluid.droppedSeconds,0);assert.equal(r.frameGeneration.enabled,false);
    assert.equal(r.frameGeneration.presentedFrames,360);assert.equal(r.dlssEvaluations,360);
    assert.equal(r.rrHistoryResets,1);assert.equal(r.photonCounters[5],0);assert.equal(r.photonCounters[6],0);
    for(const row of r.samplesMs){assert.equal(row.length,r.timingColumns.length);assert.ok(row.every(x=>Number.isFinite(x)&&x>=0));}
  }
  const rows=reports.flatMap(r=>r.samplesMs),columns=reports[0].timingColumns;
  const medianMs=Object.fromEntries(columns.map((name,i)=>[name,quantile(rows.map(r=>r[i]),.5)]));
  return {names,samples:rows.length,medianMs,frameP95Ms:quantile(rows.map(r=>r[5]),.95),
    reciprocalMedianRenderFps:1000/medianMs.frame,
    perRun:reports.map(r=>({medianMs:r.medianMs,frameP95Ms:quantile(r.samplesMs.map(x=>x[5]),.95)}))};
}
function parity(kind){
  const a=readCapture(join(folder,`fluid-parity-before-${kind}`));
  const b=readCapture(join(folder,`fluid-parity-after-${kind}`));
  const channels=a.channels.map((channel,i)=>{
    const x=channel.values,y=b.channels[i].values;assert.equal(x.length,y.length);
    let error=0,magnitude=0,max=0;
    for(let j=0;j<x.length;j++){const d=Math.abs(x[j]-y[j]);error+=d;magnitude+=Math.abs(x[j]);max=Math.max(max,d);}
    return {index:i,relativeL1:error/Math.max(magnitude,1e-30),maxAbs:max};
  });
  // Static scalar fixtures isolate rendering from chaotic long-running APIC
  // summation-order drift. RR is temporal/learned and is not bit-identical.
  for(const c of channels)assert.ok(c.relativeL1<(c.index===7?.005:c.index===1?.001:.00001),`${kind} channel ${c.index} changed`);
  if(kind!=='play'){
    assert.equal(a.report.fluidProbes.hits,b.report.fluidProbes.hits);
    assert.equal(b.report.fluidProbes.glassShellHits,10);
    assert.equal(b.report.fluidProbes.badRoots,0);assert.equal(b.report.fluidProbes.truncated,0);
  }
  return {kind,channels,beforeProbes:a.report.fluidProbes,afterProbes:b.report.fluidProbes};
}
const comparisons=['pool','play'].map(view=>{
  const before=profiles('baseline',view),after=profiles('optimized',view);
  return {view,before,after,frameTimeReduction:1-after.medianMs.frame/before.medianMs.frame,
    renderFpsGain:before.medianMs.frame/after.medianMs.frame-1};
});
const shaderNames=['Transport-0-1','Transport-1-1','Transport-2-1','Clear','FluidSurfaceBounds','FluidJacobi','FluidDensityJacobi'];
console.log(JSON.stringify({baselineCommit:'035bf21ab2b967c4da9feba191b362b85142466f',
  context:'RTX 5090, serial unpaced 1080p Balanced, 100k particles, 120 Hz APIC, 120 pressure/60 density iterations, two substeps per real frame, FG off; 32 warmup + 328 samples/run. Renderer frame interval includes submission/present/fence, excludes outer game update. Median columns need not sum.',
  comparisons,
  // This control already has the new timestamp columns and FAST_TRACE BLAS
  // preference but retains every ORIGINAL fluid compute shader. It isolates
  // solver GPU time, not baseline camera/frame performance.
  originalSolverTimingControl:['pool','play'].map(view=>profiles('render',view)),
  parity:['sphere','sheet','play'].map(parity),
  optimizedShaderSha256:Object.fromEntries(shaderNames.map(n=>[n,createHash('sha256').update(readFileSync(join(folder,'shaders',`${n}.dxil`))).digest('hex')]))
},null,2));
