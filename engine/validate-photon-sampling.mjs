// First run test-photon-sampling.ps1. Optional reference is a 240-frame flowing
// room capture from before the correction, at the same 65536-photon budget.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {join} from 'node:path';
import {createHash} from 'node:crypto';
import {readCapture} from './validate-captures.mjs';
const [folder,referencePrefix]=process.argv.slice(2);
if(!folder)throw new Error('Usage: node engine/validate-photon-sampling.mjs runtime-directory [reference-prefix]');
function quietFloor(capture){
  const atlas=capture.channels[9];assert.equal(atlas.header[1],1280);
  let mean=0,squared=0,count=0;
  // Fixed floor-chart patch away from receivers, laser endpoints and the inlet.
  // Use raw XYZ Y, before albedo, view projection, RR or Frame Generation.
  for(let y=16;y<80;y++)for(let x=176;x<240;x++){
    const value=atlas.values[(y*1280+x)*4+1];mean+=value;squared+=value*value;count++;
  }
  mean/=count;
  return {meanY:mean,coefficientOfVariation:Math.sqrt(squared/count-mean*mean)/mean};
}
const cases=['flow','still','fixed','dxr','nvapi','frame-gen','restir'];
let flow;
const runs=cases.map(name=>{
  const capture=readCapture(join(folder,`photon-sampling-${name}`)),r=capture.report;
  assert.equal(r.frames,r.dlssEvaluations);assert.equal(r.photonsPerFrame,65536);
  assert.equal(r.fluidProbes.badRoots,0);assert.equal(r.fluidProbes.truncated,0);
  assert.ok(r.fluidProbes.hits>0&&r.fluid.validated&&r.waterPhotonEntries>0);
  assert.equal(r.restirPT.invalidLastFrame,0);assert.equal(r.rrHistoryResets,1);
  if(name==='fixed'){assert.equal(r.floatAtomics,false);assert.equal(r.hitMode,0);}
  if(name==='dxr')assert.equal(r.hitMode,2);
  if(name==='nvapi')assert.equal(r.hitMode,1);
  if(name==='frame-gen')assert.ok(r.frameGeneration.extraPresents>40&&r.frameGeneration.status===0);
  if(name==='restir')assert.ok(r.restirPT.enabled&&r.restirPT.pixelsLastFrame>0);
  if(name==='flow'){
    flow=capture;assert.equal(r.frames,240);
    assert.ok(quietFloor(capture).coefficientOfVariation<.14,'Structured dark patches remain in the quiet water atlas');
  }
  return {name,frames:r.frames,medianMs:r.medianMs,energyWatts:r.photonEnergyWatts,
    footprintCapped:r.photonCounters[7],rawCapture:capture.summary};
});
let comparison;
if(referencePrefix){
  const reference=readCapture(referencePrefix),r=reference.report;
  assert.equal(r.frames,240);assert.equal(r.photonsPerFrame,flow.report.photonsPerFrame);
  assert.equal(r.waterFloodWatts,flow.report.waterFloodWatts);
  const before=quietFloor(reference),after=quietFloor(flow);
  assert.ok(Math.abs(after.meanY/before.meanY-1)<.03,'Quiet floor mean energy changed');
  assert.ok(after.coefficientOfVariation<before.coefficientOfVariation*.65,'Insufficient pattern reduction');
  comparison={referencePrefix,before,after,reductionPercent:100*(1-after.coefficientOfVariation/before.coefficientOfVariation)};
}
console.log(JSON.stringify({context:'RTX 5090, 1280x720 Balanced, 65536 spectral photons/frame. Raw caustics checked before albedo/DLSS. GPU probe runs are not representative performance benchmarks. Surface, solver, light flux and temporal history are unchanged.',
  quietFloor:quietFloor(flow),comparison,runs,
  shaderSha256:Object.fromEntries([0,1,2].flatMap(mode=>[0,1].map(atomic=>{
    const name=`Transport-${mode}-${atomic}`;
    return [name,createHash('sha256').update(readFileSync(join(folder,'shaders',`${name}.dxil`))).digest('hex')];
  })))},null,2));
