// Read-only collector; save the checked JSON with apply_patch. Reuse the full
// precision/geometry/optical acceptance checks, then independently check raw A/B.
import {readFileSync, statSync} from 'node:fs';
import {join} from 'node:path';
import {execFileSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
import {readCapture} from './validate-captures.mjs';

const folder=process.argv[2];
if(!folder || process.argv.length!==3) throw Error('Usage: node engine/record-capacity-schedule.mjs RUNTIME');
const correctness=JSON.parse(execFileSync(process.execPath,['engine/record-capacity-precision.mjs',folder],
  {encoding:'utf8',maxBuffer:4*1024*1024}));
const precisionCheckpoint=JSON.parse(readFileSync('engine/capacity-precision-validation.json','utf8'));
assert.deepEqual(correctness.shaderSha256,precisionCheckpoint.shaderSha256,
  'Scheduling-only acceptance requires the unchanged precision checkpoint shaders');
for(const c of correctness.cases){
  assert.equal(c.carrier.pressureCyclesPerIteration,1);
  assert.equal(c.carrier.maxOuterIterations,256);
  assert.equal(c.carrier.recordedMultigridCycles,c.steps*256);
  assert.equal(c.carrier.multigridCycles,c.carrier.iterations);
}
const hash=p=>createHash('sha256').update(readFileSync(p)).digest('hex');
const built=statSync(join(folder,'NVMatrixFluidLab.exe')).mtimeMs;
const median=v=>[...v].sort((a,b)=>a-b)[Math.floor(v.length/2)];
let referenceSettings;
const profiles=[];
for(let repeat=1;repeat<=3;repeat++) for(const cycles of (repeat%2?[4,1]:[1,4])){
  const name=`capacity-cost-paired${cycles}-${repeat}`,prefix=join(folder,name);
  for(const ext of ['.json','.inputs','.ppm']) assert.ok(statSync(prefix+ext).mtimeMs>=built,`Stale ${name}${ext}`);
  const {report:r,summary:optical}=readCapture(prefix);
  const f=r.fluid,b=f.bulk,p=b.carrierProjection,i=b.implicitTransport,m=f.adaptiveMac,c=f.cutCells,s=f.bulkPressure;
  assert.equal(r.frames,120);assert.equal(r.sampleCount,88);assert.equal(r.warmupFrames,32);
  assert.equal(r.samplesMs.length,88);assert.equal(f.steps,240);assert.equal(r.dlssEvaluations,120);
  assert.ok(r.orbitTest && f.roomPool && !r.frameGeneration.enabled && !f.droppedSeconds);
  for(const part of [f,b,m,c,s,i,p]) assert.ok(!part.validated && !(part.auditedFrames||part.auditedIdleFrames));
  assert.ok(p.mixedPressureCoupled && c.timeCenteredPressure && m.cutPressure);
  assert.equal(p.pressureCyclesPerIteration,cycles);assert.equal(p.maxOuterIterations,256);
  assert.equal(p.multigridCycles,p.iterations*cycles);assert.equal(p.recordedMultigridCycles,240*256*cycles);
  for(const part of [b,p,i,s,c,m]) assert.equal(part.invalid,0);
  assert.equal(p.exhaustedSteps+p.isolatedRows+i.exhaustedSteps+m.multigrid.exhaustedSolves,0);
  assert.equal(p.steps,240);assert.equal(i.steps,240);assert.equal(s.projectionCalls,240);
  assert.ok(p.peakResidual<=5.01e-7 && m.multigrid.peakFinalDivergence<=.000101);
  assert.equal(b.inventoryBits,64);assert.equal(i.inventoryBits,64);assert.equal(c.coarseCapacityBits,64);
  assert.ok(i.peakResidual<=2.01e-13 && i.peakExcessM3<=1e-11 && !c.bulkInClosedCellsM3);
  assert.ok(b.relativeVolumeError<=.0002 && b.maxVolumeFraction<=1.00001);
  assert.equal(r.photonCounters[5]+r.photonCounters[6]+r.fluidProbes.badRoots+r.fluidProbes.truncated,0);
  const settings={adapter:r.adapter,internal:[r.internalWidth,r.internalHeight],output:[r.outputWidth,r.outputHeight],
    quality:r.dlssMode,photons:r.photonsPerFrame,history:r.historyLength,ser:r.serActive,atomics:r.floatAtomics,
    transfer:f.transfer,initial:f.initialParticles,capacity:f.capacity,particles:f.particles,emitted:f.emittedParticles,
    grid:f.gridCells,density:f.density,viscosity:f.kinematicViscosity,tension:f.surfaceTension,
    deterministicBins:f.deterministicBins,whitewater:!!r.whitewater};
  if(referenceSettings) assert.deepEqual(settings,referenceSettings);else referenceSettings=settings;
  assert.ok(r.medianMs.every(Number.isFinite) && r.samplesMs.every(v=>v.every(Number.isFinite)));
  profiles.push({name,repeat,cycles,reportSha256:hash(prefix+'.json'),captureSha256:hash(prefix+'.inputs'),
    rawMedianMs:r.medianMs[5],fluidMedianMs:r.medianMs[6],cameraMedianMs:r.medianMs[2],photonsMedianMs:r.medianMs[0],
    reconstructionMedianMs:r.medianMs[7],capacityMeanMs:p.meanFrameMs,
    outerIterations:p.iterations,multigridCycles:p.multigridCycles,recordedMultigridCycles:p.recordedMultigridCycles,
    pressurePeakResidual:p.peakResidual,physicalPeakDivergence:m.multigrid.peakFinalDivergence,
    implicitPeakResidual:i.peakResidual,transportPeakExcessM3:i.peakExcessM3,optical});
}
const summary={};
for(const field of ['rawMedianMs','fluidMedianMs','capacityMeanMs','cameraMedianMs','photonsMedianMs']){
  const reference=median(profiles.filter(p=>p.cycles===4).map(p=>p[field]));
  const optimized=median(profiles.filter(p=>p.cycles===1).map(p=>p[field]));
  summary[field]={reference,optimized,reductionPercent:100*(1-optimized/reference)};
}
console.log(JSON.stringify({schema:1,generatedUtc:new Date().toISOString(),
  scope:'One V-cycle per capacity outer iteration; unchanged final physical and signed phase gates',
  precisionCheckpointShadersUnchanged:true,
  readyForDefault:false,performanceScope:'120-frame room/emitter/orbit, 1080p balanced, FG off, three alternating pairs; not general real-time acceptance',
  referenceSettings,summary,profiles,correctness},null,2));
