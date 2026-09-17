import assert from 'node:assert/strict';
import {join} from 'node:path';
import {readCapture} from './validate-captures.mjs';
const [folder,prefix='temporal']=process.argv.slice(2);
if(!folder)throw new Error('Usage: node engine/validate-temporal.mjs capture-directory');
const frames=[64,96,160,192,253,254,255,256];
const captures=new Map(frames.map(frame=>{
  const c=readCapture(join(folder,`${prefix}-${frame}`),frame===96?1:32);
  assert.equal(c.report.rrHistoryResets,1);
  assert.equal(c.report.dlssEvaluations,frame);
  assert.equal(c.report.historyResets,frame===64?1:frame===96?33:34);
  assert.equal(c.report.photonCounters[5],0);
  assert.equal(c.report.photonCounters[6],0);
  return [frame,c];
}));
assert.ok(captures.get(96).summary.motionMax>.01,'moving prism needs dense motion guides');
assert.ok(captures.get(192).summary.motionMax>.01,'orbit needs camera motion guides');
for(const frame of [64,160,253,254,255,256])
  assert.ok(captures.get(frame).summary.motionMax<.001,`static motion includes jitter at ${frame}`);

// Compare consecutive, fully settled HDR frames in display-like luminance.
// Average RR's native-resolution samples into the corresponding input pixel;
// exclude silhouettes/depth edges where subpixel coverage legitimately changes.
const luma=(v,i)=>Math.max(0,.2126*v[i]+.7152*v[i+1]+.0722*v[i+2]);
const compress=x=>x/(1+x);
function imageDelta(first,second){
  const a=captures.get(first).channels[7].values,b=captures.get(second).channels[7].values;
  let sum=0;
  for(let i=0;i<a.length;i+=4)sum+=Math.abs(compress(luma(a,i))-compress(luma(b,i)));
  return sum/(a.length/4);
}
// Low frame deltas alone would also accept a frozen output. Require response to
// both transport and camera motion, then convergence back to the original view.
const prismResponse=imageDelta(64,96),orbitResponse=imageDelta(160,192),returnError=imageDelta(64,256);
assert.ok(prismResponse>.001,'RR output did not respond to prism movement');
assert.ok(orbitResponse>.001,'RR output did not respond to orbit');
assert.ok(returnError<Math.min(prismResponse,orbitResponse)*.2,'RR retained stale lighting/view history');
function outputLuma(channel,x,y,w,h){
  const [,ow,oh]=channel.header,v=channel.values;
  const left=Math.floor(x*ow/w),right=Math.floor((x+1)*ow/w);
  const top=Math.floor(y*oh/h),bottom=Math.floor((y+1)*oh/h);
  let sum=0,count=0;
  for(let yy=top;yy<bottom;yy++)for(let xx=left;xx<right;xx++){
    sum+=luma(v,(yy*ow+xx)*4);count++;
  }
  return compress(sum/count);
}
let noisyDelta=0,rrDelta=0,count=0;
for(const frame of [254,255,256]){
  const a=captures.get(frame-1).channels,b=captures.get(frame).channels;
  const [,w,h]=a[0].header;
  for(let y=2;y<h-2;y++)for(let x=2;x<w-2;x++){
    const p=y*w+x,d=a[2].values[p],d2=b[2].values[p];
    if(d>=199||Math.abs(d-d2)>.02)continue;
    if([p-1,p+1,p-w,p+w].some(q=>Math.abs(a[2].values[q]-d)>.05))continue;
    const noisyA=compress(luma(a[0].values,p*4)),noisyB=compress(luma(b[0].values,p*4));
    noisyDelta+=Math.abs(noisyA-noisyB);
    rrDelta+=Math.abs(outputLuma(a[7],x,y,w,h)-outputLuma(b[7],x,y,w,h));
    count++;
  }
}
assert.ok(count>1000,'not enough stable receiver pixels');
const ratio=rrDelta/Math.max(noisyDelta,1e-12);
assert.ok(noisyDelta>0,'missing noisy input signal');
assert.ok(ratio<.75,`RR is not stabilizing settled lighting: delta ratio ${ratio}`);
console.log(JSON.stringify({frames:256,rrHistoryResets:1,atlasResets:34,stableSamples:count,
  meanNoisyLuminanceDelta:noisyDelta/count,meanRrLuminanceDelta:rrDelta/count,rrToNoisyDeltaRatio:ratio,
  prismResponse,orbitResponse,returnError},null,2));
console.log('PASS: finite guides, independent atlas/RR history, motion, settling and temporal reconstruction.');
