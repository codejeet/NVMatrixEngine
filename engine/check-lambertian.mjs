import {statSync} from 'node:fs';
import {join} from 'node:path';
import assert from 'node:assert/strict';
import {readCapture} from './validate-captures.mjs';
const [folder,...requested]=process.argv.slice(2);
if(!folder)throw new Error('Usage: node engine/check-lambertian.mjs <runtime folder> [scenes...]');
const frames=[128,144,160,176,192,208,224,240,256];
const scenes=requested.length?requested:['diffuse','water','underwater','fresh-pt','reused-pt','temporal-pt'];
const built=Math.max(statSync(join(folder,'NVMatrixFluidLab.exe')).mtimeMs,statSync(join(folder,'shaders/Transport-0-0.dxil')).mtimeMs);
const results=[];
for(const scene of scenes) {
  let reducedSum,referenceSum,reduced,reference,rawError=0,rawEnergy=0,maxRootError=0;
  const rgb=[0,0,0],referenceRgb=[0,0,0];
  for(const frame of frames) {
    const prefix=mode=>join(folder,`lambertian-${scene}-${mode}-${frame}`);
    for(const mode of ['reduced','reference'])assert.ok(statSync(`${prefix(mode)}.json`).mtimeMs>=built,'Stale optical captures');
    const a=readCapture(prefix('reduced')),b=readCapture(prefix('reference'));
    reduced=a.report;reference=b.report;
    assert.equal(reduced.lambertianReduction,true);assert.equal(reference.lambertianReduction,false);
    assert.equal(reduced.transportHash,reference.transportHash,'Different transport state');
    assert.equal(reduced.opticalImportance.totals[5],reference.opticalImportance.totals[5],'Different path budgets');
    const x=a.channels[0].values,y=b.channels[0].values;
    assert.equal(x.length,y.length);
    reducedSum??=new Float64Array(x.length);referenceSum??=new Float64Array(y.length);
    for(let i=0;i<x.length;i++)if(i%4!==3) {
      reducedSum[i]+=x[i]/frames.length;referenceSum[i]+=y[i]/frames.length;
      rgb[i%4]+=x[i]/(x.length/4*frames.length);referenceRgb[i%4]+=y[i]/(y.length/4*frames.length);
      rawError+=(x[i]-y[i])**2;rawEnergy+=y[i]**2;
    }
    // The light-side solution and immutable guides must be unchanged. Motion
    // permits sub-1/8192-pixel FP scheduling differences at nearly zero velocity.
    for(const channel of [1,2,3,4,5,6,8,9])if(a.channels[channel]) {
      const x=a.channels[channel].values,y=b.channels[channel].values;
      let max=0;for(let i=0;i<x.length;i++)max=Math.max(max,Math.abs(x[i]-y[i]));
      assert.ok(max<=(channel===1?1/8192:0),`${scene}: guide/atlas ${channel} differs by ${max}`);
    }
    if(scene==='water'||scene==='underwater') {
      assert.ok(statSync(`${prefix('roots')}.json`).mtimeMs>=built,'Stale water reference');
      const roots=readCapture(prefix('roots'));
      assert.equal(roots.report.waterVisibilityBracket,true);assert.equal(reference.waterVisibilityBracket,false);
      assert.equal(roots.report.lambertianReduction,false);
      for(let channel=0;channel<b.channels.length;channel++)if(channel!==7) {
        const z=roots.channels[channel].values,v=b.channels[channel].values;
        for(let i=0;i<z.length;i++)maxRootError=Math.max(maxRootError,Math.abs(z[i]-v[i]));
      }
      assert.equal(maxRootError,0,'Water boolean traversal changed raw lighting, guides or photon atlas');
    }
  }
  const relativeRgb=rgb.map((v,c)=>v/referenceRgb[c]-1);
  if(scene==='reused-pt')assert.ok(reduced.restirPT.reusedLastFrame>0,'Missing spatial PT reuse');
  if(scene==='temporal-pt')assert.ok(reduced.restirPT.temporalFrames>0&&reduced.restirPT.reusedLastFrame>0,'Missing temporal PT reuse');
  assert.ok(relativeRgb.every(v=>Math.abs(v)<.02),`${scene}: mean RGB changed by more than 2%`);
  let error=0,energy=0;
  for(let i=0;i<reducedSum.length;i++)if(i%4!==3){error+=(reducedSum[i]-referenceSum[i])**2;energy+=referenceSum[i]**2;}
  results.push({scene,relativeRgb,rawRelativeRms:Math.sqrt(rawError/rawEnergy),meanRelativeRms:Math.sqrt(error/energy),
    adapter:reduced.adapter,internalResolution:[reduced.internalWidth,reduced.internalHeight],
    outputResolution:[reduced.outputWidth,reduced.outputHeight],frames:reduced.frames,
    cameraRaysReduced:reduced.opticalImportance.totals[0],cameraRaysReference:reference.opticalImportance.totals[0],
    cameraMsReduced:reduced.medianMs[2],cameraMsReference:reference.medianMs[2],
    frameMsReduced:reduced.medianMs[5],frameMsReference:reference.medianMs[5],
    temporalReuseFrames:reduced.restirPT.temporalFrames,maxRootError});
}
console.log(JSON.stringify({passed:true,scope:'Same-binary raw radiance, nine seeded frames; 2% mean RGB gate, exact material/depth/normal/distance guides and photons, motion tolerance 1/8192 pixel, exact water visibility parity. Timings are observations, not universal speed claims.',results},null,2));
