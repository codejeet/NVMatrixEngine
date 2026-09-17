// Run test-room-water.ps1 first. Validates raw RR guides and produces a compact
// reproducible report; capture buffers/build products remain outside the repo.
import {readFileSync} from 'node:fs';
import {join} from 'node:path';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
import {readCapture} from './validate-captures.mjs';
const folder=process.argv[2];
if(!folder)throw new Error('Usage: node engine/validate-room-water.mjs runtime-directory');
const cases=['still','flow','flow-no-whitewater','inlet-view','controls','capacity','already-full','fixed','nvapi','frame-gen','long-flow'];
const quantile=(xs,q)=>[...xs].sort((a,b)=>a-b)[Math.floor(q*(xs.length-1))];
const runs=cases.map(name=>{
  const {report:r,summary}=readCapture(join(folder,`room-water-${name}`));
  assert.ok(r.fluid.roomPool&&r.fluid.validated);assert.equal(r.fluid.active,100000+r.fluid.emittedParticles);
  assert.equal(r.dlssEvaluations,r.frames);assert.equal(r.fluid.droppedSeconds,0);
  assert.ok(r.fluid.maxSolidPenetration<.001);
  if (r.fluid.cuda?.mixedPressure) {
    assert.equal(r.fluid.pressureAudit, 'published-fine-face-flux');
    assert.equal(r.fluid.maxPressureResidualMismatch, null);
    assert.ok(Number.isFinite(r.fluid.maxPublishedFluxDivergence) && r.fluid.maxPublishedFluxDivergence <= .000101);
    assert.ok(Number.isFinite(r.fluid.maxPublishedFluxMismatch) && r.fluid.maxPublishedFluxMismatch <= .00001);
    assert.equal(r.fluid.cuda.rejectedFrames, 0);
    assert.equal(r.fluid.cuda.pressureCaps, 0);
  } else {
    assert.ok(Number.isFinite(r.fluid.maxPressureResidualMismatch) && r.fluid.maxPressureResidualMismatch < .0001);
  }
  assert.equal(r.fluid.roomQuadrants.reduce((a,b)=>a+b),r.fluid.active);
  assert.ok(r.fluid.roomQuadrants.every(n=>n>15000));
  assert.equal(r.fluidProbes.badRoots,0);assert.equal(r.fluidProbes.truncated,0);assert.ok(r.fluidProbes.hits>0);
  const enabled=!['still','flow-no-whitewater'].includes(name);
  assert.equal(!!r.whitewater,enabled);
  if(enabled){
    assert.ok(r.whitewater.validated);assert.equal(r.whitewater.invalid,0);
    assert.ok(r.whitewater.foam+r.whitewater.bubbles+r.whitewater.spray<=r.whitewater.capacity);
    assert.equal(r.whitewaterProbes.bad,0);
    if(!['capacity','already-full'].includes(name)){
      assert.ok(r.whitewaterProbes.foam>0);assert.ok(r.whitewaterProbes.bubbles>0);
    }
  }
  assert.equal(r.frameGeneration.status,0);
  if(name==='frame-gen')assert.ok(r.frameGeneration.enabled&&r.frameGeneration.extraPresents>40);
  else assert.equal(r.frameGeneration.extraPresents,0);
  for(const row of r.samplesMs){assert.equal(row.length,11);assert.ok(row.every(x=>Number.isFinite(x)&&x>=0));}
  return {name,frames:r.frames,particles:r.fluid.active,volume:r.fluid.estimatedOccupiedVolume,
    restVolume:r.fluid.restVolume,renderedVolume:r.fluidSurface.tetrahedralVolumeEstimate,
    roomQuadrants:r.fluid.roomQuadrants,whitewater:r.whitewater??null,whitewaterProbes:r.whitewaterProbes??null,
    medianMs:Object.fromEntries(r.timingColumns.map((c,i)=>[c,r.medianMs[i]])),
    frameP95Ms:quantile(r.samplesMs.map(row=>row[5]),.95),frameGeneration:r.frameGeneration,capture:summary};
});
const shaderNames=['Transport-0-1','Transport-1-1','Transport-2-1','FluidInitialize','FluidEmit','FluidCollide','WhitewaterClear','WhitewaterUpdate'];
console.log(JSON.stringify({context:'RTX 5090; 1280x720 Balanced; 100k initial APIC, 120 Hz, two substeps/rendered frame, 120 pressure/60 density iterations. FG off except explicit FG test. 32-frame warmup. Bounded validation includes root probes; medians are not guaranteed interactive frame rates.',
  runs,shaderSha256:Object.fromEntries(shaderNames.map(name=>[name,createHash('sha256').update(readFileSync(join(folder,'shaders',`${name}.dxil`))).digest('hex')]))},null,2));
