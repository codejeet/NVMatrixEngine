// Read-only evidence collector. Reject stale binaries/captures; use apply_patch
// to save the resulting record. This script never launches GPU workloads.
import {readFileSync,statSync,readdirSync} from 'node:fs';
import {join} from 'node:path';
import {execFileSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
const folder=process.argv[2];
if(!folder)throw new Error('Usage: node engine/record-cut-cells.mjs <runtime folder>');
const shaders=join(folder,'shaders'),exe=join(folder,'NVMatrixFluidLab.exe');
const names=readdirSync(shaders).filter(n=>n.endsWith('.dxil')).sort();
const built=Math.max(statSync(exe).mtimeMs,...names.map(n=>statSync(join(shaders,n)).mtimeMs));
function read(name){const path=join(folder,`${name}.json`);assert.ok(statSync(path).mtimeMs>=built,`Stale ${name}`);return JSON.parse(readFileSync(path));}
const expected={empty:4,plane:8,oblique:8,'moving-plane':60,sphere:8,pit:48,room:48,wake:180,controls:12,view:16,'fg-view':16,adaptive:48};
const cases=Object.entries(expected).map(([name,frames])=>{
  const r=read(`cut-cells-${name}`),c=r.fluid.cutCells;
  assert.equal(r.frames,frames);assert.equal(c.auditedFrames,frames);assert.ok(c.validated&&r.fluid.validated);
  assert.equal(c.invalid+c.maxHistoryError+r.photonCounters[5]+r.photonCounters[6]+(r.fluidProbes?.badRoots??0)+(r.fluidProbes?.truncated??0),0);
  if(name==='fg-view')assert.ok(!r.frameGeneration.enabled);
  if(name==='moving-plane')assert.equal(c.updates,r.fluid.steps);
  if(['plane','oblique','sphere','empty'].includes(name))assert.equal(c.updates,1);
  return {name,frames,steps:r.fluid.steps,capacity:c,bulk:name==='room'?r.fluid.bulk:undefined};
});
const parity=JSON.parse(execFileSync(process.execPath,[join(import.meta.dirname,'check-cut-cells-parity.mjs'),folder],{encoding:'utf8'}));
const profiles=[];
for(let repeat=1;repeat<=3;repeat++)for(const mode of ['off','on']){
  const r=read(`cut-cost-${mode}-${repeat}`),c=r.fluid.cutCells;
  assert.equal(r.frames,300);assert.equal(r.sampleCount,268);assert.equal(r.fluid.steps,600);
  assert.ok(!r.frameGeneration.enabled&&!r.fluid.validated&&!r.fluid.deterministicBins&&!c?.auditedFrames);
  assert.equal(!!c,mode==='on');
  profiles.push({mode,repeat,rawMedianMs:r.medianMs[5],fluidMedianMs:r.medianMs[6],capacityMeanMs:c?.meanFrameMs??0,geometryUpdates:c?.updates??0});
}
const median=a=>a.sort((x,y)=>x-y)[a.length>>1];
const performance={};
for(const mode of ['off','on']){
  const rows=profiles.filter(r=>r.mode===mode);
  performance[mode]={rawMs:median(rows.map(r=>r.rawMedianMs)),fluidMs:median(rows.map(r=>r.fluidMedianMs)),capacityMeanMs:median(rows.map(r=>r.capacityMeanMs))};
}
const hash=createHash('sha256');for(const name of names){hash.update(name);hash.update(readFileSync(join(shaders,name)));}
console.log(JSON.stringify({schema:1,date:new Date().toISOString(),baseCommit:'4e1ec33',gpu:'NVIDIA RTX 5090',
  scope:'Opt-in sampled solid cut-cell geometry and passive capacity audit. No authoritative flowing bulk or cut-cell pressure/transport yet; full adaptive goal remains incomplete.',
  nodeTests:121,windowsCTests:7,cases,
  parity:{passed:parity.passed,scenes:parity.checks.map(c=>({scene:c.scene,identicalChannels:c.channels.length}))},
  profiles,performance,exeSha256:createHash('sha256').update(readFileSync(exe)).digest('hex'),shaderSetSha256:hash.digest('hex'),
  debugLayer:{gpuBasedValidationExecuted:false,previousInitializationError:'0x887A002D'},
  frameGeneration:'Only overlay suspension is checked here. Actual generated-present acceptance remains unresolved from the previous checkpoint.'},null,2));
