import {statSync,readdirSync} from 'node:fs';
import {join} from 'node:path';
import assert from 'node:assert/strict';
import {readCapture} from './validate-captures.mjs';
const folder=process.argv[2];
if(!folder)throw new Error('Usage: node engine/check-optical-estimator.mjs <runtime folder>');
const frames=[128,144,160,176,192,208,224,240,256];
const built=Math.max(statSync(join(folder,'NVMatrixFluidLab.exe')).mtimeMs,
  ...readdirSync(join(folder,'shaders')).filter(n=>n.endsWith('.dxil')).map(n=>statSync(join(folder,'shaders',n)).mtimeMs));
function sequence(scene,mode){
  let mean,report;const rgbFrames=[];
  for(const frame of frames){
    const path=join(folder,`optical-estimator-${scene}-${mode}-${frame}`);
    assert.ok(statSync(`${path}.json`).mtimeMs>=built,`Stale ${path}`);
    const c=readCapture(path);report=c.report;const v=c.channels[0].values;
    mean??=new Float64Array(v.length);const rgb=[0,0,0];
    for(let i=0;i<v.length;i+=4)for(let j=0;j<3;j++){mean[i+j]+=v[i+j]/frames.length;rgb[j]+=v[i+j]/(v.length/4);}
    rgbFrames.push(rgb);
    if(scene==='reused-pt')assert.ok(report.restirPT.temporalFrames>0&&report.restirPT.reusedLastFrame>0);
  }
  const rgb=[0,1,2].map(j=>rgbFrames.reduce((sum,v)=>sum+v[j],0)/frames.length);
  return {mean,rgb,rgbFrames,report};
}
const results=[];
for(const scene of ['diffuse','fresh-pt','reused-pt','water']){
  const reference=sequence(scene,'reference');
  for(const mode of ['adaptive','uniform']){
    const c=sequence(scene,mode),relativeRgb=c.rgb.map((v,j)=>v/reference.rgb[j]-1);
    let error=0,energy=0;
    for(let i=0;i<c.mean.length;i++)if(i%4!==3){error+=(c.mean[i]-reference.mean[i])**2;energy+=reference.mean[i]**2;}
    // Empirical brightness gate, not a formal proof for a reused estimator.
    // Raw means from nine seeded production frames; no RR, clipping or exposure matching.
    assert.ok(relativeRgb.every(v=>Math.abs(v)<.02),`${scene}/${mode}: raw RGB mean differs by >2%`);
    results.push({scene,mode,frames,meanRgb:c.rgb,referenceRgb:reference.rgb,relativeRgb,
      rawRelativeRms:Math.sqrt(error/Math.max(energy,1e-30)),
      temporalReuseFrames:c.report.restirPT.temporalFrames,
      selectedSamples:c.report.opticalImportance.totals[5],cameraRays:c.report.opticalImportance.totals[0]});
  }
}
console.log(JSON.stringify({passed:true,
  scope:'Empirical raw-radiance mean check against uniform eight-sample reference. Does not establish general unbiasedness, specular reservoir reuse, or a ground-truth error bound.',results},null,2));
