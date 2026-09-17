// Read-only evidence collector: rejects reports older than the current binary
// or shader set. Save stdout using apply_patch after every check has completed.
import {readFileSync,statSync,readdirSync} from 'node:fs';
import {join} from 'node:path';
import {execFileSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
const folder=process.argv[2];
if(!folder)throw new Error('Usage: node engine/record-optical.mjs <runtime folder>');
const shaders=join(folder,'shaders'),exe=join(folder,'NVMatrixFluidLab.exe');
const names=readdirSync(shaders).filter(n=>n.endsWith('.dxil')).sort();
const built=Math.max(statSync(exe).mtimeMs,...names.map(n=>statSync(join(shaders,n)).mtimeMs));
function read(name){const path=join(folder,`${name}.json`);assert.ok(statSync(path).mtimeMs>=built,`Stale ${name}`);return JSON.parse(readFileSync(path));}
function check(script,args){return JSON.parse(execFileSync(process.execPath,[join(import.meta.dirname,script),folder,...args],{encoding:'utf8',maxBuffer:16*1024*1024}));}
const expected={observe:48,adaptive:48,uniform:48,opaque:48,pt:48,orbit:96,rolling:300,inlet:64,
  view:48,receivers:48,feedback:48,controls:48,freeze:48,cap1:48,cap8:16,fg:48,nvapi:48,dxr:48,fixed:48};
const cases=Object.entries(expected).map(([name,frames])=>{
  const r=read(`optical-${name}`),o=r.opticalImportance;
  assert.equal(r.frames,frames);assert.equal(o.audits,name==='controls'?12:frames);
  assert.equal(r.photonCounters[5]+r.photonCounters[6]+r.restirPT.invalidLastFrame+o.last[12],0);
  if(name==='feedback')assert.ok(o.feedbackAudits&&r.complexity.validated&&r.complexity.opticalRaisedBricks>0);
  return {name,frames,audits:o.audits,feedbackAudits:o.feedbackAudits,last:o.last,
    opticalFeedbackBricks:r.complexity?.opticalFeedbackBricks??0,opticalRaisedBricks:r.complexity?.opticalRaisedBricks??0,
    allocatedBytes:o.allocatedBytes,classificationMedianMs:o.classificationMedianMs};
});
const parity=check('check-optical-parity.mjs',[]),estimator=check('check-optical-estimator.mjs',[]);
for(const prefix of ['water-temporal-optical-adaptive','water-temporal-optical-uniform'])
  for(const f of [128,176,224,272,320])assert.equal(read(`${prefix}-${f}`).rrHistoryResets,1);
const temporal=check('validate-water-temporal.mjs',['water-temporal-optical-adaptive','water-temporal-optical-uniform','--preserve']);
const profiles=[];
for(let repeat=1;repeat<=3;repeat++)for(const mode of ['cached','retraced','adaptive','uniform']){
  const r=read(`optical-perf-${mode}-${repeat}`),o=r.opticalImportance;
  assert.equal(r.frames,300);assert.equal(r.sampleCount,268);assert.equal(r.fluid.steps,600);
  assert.ok(!r.frameGeneration.enabled&&!r.fluid.validated&&!r.fluid.deterministicBins&&!o?.audits);
  const times=r.samplesMs.map(v=>v[5]).sort((a,b)=>a-b);
  profiles.push({mode,repeat,cameraMedianMs:r.medianMs[2],rawMedianMs:r.medianMs[5],
    rawP95Ms:times[Math.floor(.95*(times.length-1))],simulationMedianMs:r.medianMs[6],
    surfaceMedianMs:r.medianMs[7],classificationMedianMs:o?.classificationMedianMs??0,
    actualCameraRays:o?.totals[0]??null,selectedSamples:o?.totals[5]??null});
}
const median=v=>v.toSorted((a,b)=>a-b)[Math.floor(v.length/2)];
const performance=Object.fromEntries(['cached','retraced','adaptive','uniform'].map(mode=>{
  const p=profiles.filter(v=>v.mode===mode);
  return [mode,Object.fromEntries(['cameraMedianMs','rawMedianMs','rawP95Ms','classificationMedianMs'].map(k=>[k,median(p.map(v=>v[k]))]))];
}));
const shaderHash=createHash('sha256');for(const n of names){shaderHash.update(n);shaderHash.update(readFileSync(join(shaders,n)));}
const fg=read('optical-fg').frameGeneration,baselineFg=read('frame-gen-2x').frameGeneration;
console.log(JSON.stringify({schema:1,date:new Date().toISOString(),baseCommit:'cfd89e8',gpu:'NVIDIA RTX 5090',
  scope:'Opt-in adaptive fresh optical sampling, receiver importance and interface feedback; exact primary-hit reuse. Full adaptive fluid/caustic specification remains incomplete.',
  cpuTests:117,windowsTests:7,scopedOpticalChecksPassed:true,cases,
  parity:{passed:parity.passed,comparisons:parity.checks.map(c=>({scene:c.scene,comparison:c.comparison,identicalChannels:c.channels.length}))},
  estimator,temporal,profiles,performance,
  frameGeneration:{adaptive:fg,nonAdaptive:baselineFg,actualGeneratedPresentsValidated:fg.extraPresents>0&&baselineFg.extraPresents>0,
    note:'FG reports enabled/status zero but no extra presented frames in this session, including non-adaptive baseline. Strict FG lifecycle test has not passed; do not label this full FG validation.'},
  exeSha256:createHash('sha256').update(readFileSync(exe)).digest('hex'),shaderSetSha256:shaderHash.digest('hex'),
  debugLayer:{gpuBasedValidationExecuted:false,previousInitializationError:'0x887A002D'}},null,2));
