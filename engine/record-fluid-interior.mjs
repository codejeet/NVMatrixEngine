// Summarize existing bounded GPU runs; never launch processes or rewrite results.
import {readFileSync,readdirSync} from 'node:fs';
import {createHash} from 'node:crypto';
import {join} from 'node:path';
const directory=process.argv[2];
if(!directory)throw new Error('Usage: node record-fluid-interior.mjs <runtime directory>');
const read=name=>JSON.parse(readFileSync(join(directory,`${name}.json`),'utf8'));
const names=['reference','reference-long','reference-wake','reference-uniform-wake','empty',
  'calm','cycle','calm-long','wake','fall','room','multigrid','bulk','fg','pt'];
const cases=names.map(name=>{
  const r=read(`interior-${name}`),f=r.fluid;
  return {name,frames:r.frames,steps:f.steps,validated:f.validated,active:f.active,
    restMass:f.restMassUnits,particleMass:f.particleMassUnits,interior:f.interior??null,
    volume:r.fluidSurface.tetrahedralVolumeEstimate,maxDensity:f.maxRelativeDensity,
    divergenceRms:f.divergenceRmsAfter,penetration:f.maxSolidPenetration,
    badRoots:r.fluidProbes.badRoots,truncated:r.fluidProbes.truncated,
    photonFailures:r.photonCounters.slice(5,7)};
});
const median=values=>{
  values.sort((a,b)=>a-b);
  return values.length%2?values[values.length>>1]:(values[values.length/2-1]+values[values.length/2])/2;
};
const profiles={};
for(const scene of ['calm','room']){
  profiles[scene]={};
  for(const mode of ['off','on']){
    const runs=[1,2,3].map(i=>read(`interior-cost-${scene}-${mode}-${i}`));
    const samples=runs.flatMap(r=>r.samplesMs);
    profiles[scene][mode]={frames:samples.length,
      fluidMs:median(samples.map(s=>s[6])),surfaceMs:median(samples.map(s=>s[7])),
      rawFrameMs:median(samples.map(s=>s[5])),
      runMedians:runs.map(r=>({fluid:r.medianMs[6],raw:r.medianMs[5]}))};
  }
}
const current=cases.find(c=>c.name==='wake'),reference=cases.find(c=>c.name==='reference-uniform-wake');
const shaderHash=createHash('sha256');
for(const name of readdirSync(join(directory,'shaders')).filter(n=>n.endsWith('.dxil')).sort()){
  shaderHash.update(name);shaderHash.update(readFileSync(join(directory,'shaders',name)));
}
console.log(JSON.stringify({schema:1,date:'2026-09-12',baseCommit:'ae2fcc7',gpu:'NVIDIA RTX 5090',
  scope:'Experimental dormant ownership, uniform MAC pressure; full adaptive goal incomplete',
  accepted:false,openFailure:{case:'wake',maxDensity:current.maxDensity,
    uniformReferenceMaxDensity:reference.maxDensity,limit:reference.maxDensity+.01,
    relativeVolumeError:Math.abs(current.volume/reference.volume-1)},
  nodeTests:74,windowsCTests:6,
  ordinaryFluidRegressionCases:['constant','affine','compression','roundtrip','controls','apic-long (2400 substeps)'],
  debugLayer:{available:false,previousInitializationError:'0x887A002D',gpuBasedValidationExecuted:false},
  cases,profiles,temporal:{passed:true,prefix:'water-temporal-interior',reference:'water-temporal-resampling',
    frames:320,rrResets:1,maxBrightnessChangePercent:.02894,maxRawDeltaIncreasePercent:.074754,
    maxRrDeltaIncreasePercent:.205049,
    note:'Includes refracted parallax and physical changes; not pure variance. Shallow room has no coarse owners.'},
  exeSha256:createHash('sha256').update(readFileSync(join(directory,'NVMatrixFluidLab.exe'))).digest('hex'),
  shaderSetSha256:shaderHash.digest('hex')},null,2));
