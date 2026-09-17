import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';

const hash=x=>{x^=x>>>16;x=Math.imul(x,0x7feb352d);x^=x>>>15;x=Math.imul(x,0x846ca68b);return (x^(x>>>16))>>>0;};
function sobol(i,frame){
  let x=0,y=0,vx=0x80000000,vy=0x80000000,g=i^(i>>>1);
  for(let bit=0;bit<32;bit++){
    if(g&(1<<bit)){x^=vx;y^=vy;}
    vx>>>=1;vy^=vy>>>1;
  }
  return [(x^hash(frame*3+1))>>>8,(y^hash(frame*3+2))>>>8].map(x=>x/16777216);
}
function scramble(coordinate,seed){
  const bits=Math.floor(coordinate*16777216);let prefix=1,result=0;
  for(let bit=24;bit>0;--bit){
    const input=(bits>>>(bit-1))&1;
    result=(result<<1)|(input^(hash(prefix^seed)&1));prefix=(prefix<<1)|input;
  }
  return result/16777216;
}
test('nested photon aperture scrambling retains dyadic coverage and finite [0,1) launches',()=>{
  for(const seed of [0,123,0xffffffff])for(const depth of [1,4,8,11]){
    const count=2**depth,seen=new Set();
    for(let i=0;i<count;i++){
      const s=scramble(i/count,seed);
      assert.ok(s>=0&&s<1);seen.add(Math.floor(s*count));
    }
    assert.equal(seen.size,count,'scrambling collapsed a dyadic aperture stratum');
  }
  const s=readFileSync(new URL('shaders/transport.hlsl',import.meta.url),'utf8');
  assert.match(s,/sample\.x=scramblePhotonAperture/);
  assert.match(s,/sample\.y=scramblePhotonAperture/);
  assert.match(s,/sample\.z=\(lane\+sample\.z\)\/bundle/);
  assert.match(s,/float energy=s\.power\/count/);
  assert.match(s,/\(dynamic\?16\.0:4\.0\)/);
});

// Constant irradiance through a flat, unit-transmittance pool. Reproduce launch
// sampling, per-photon Gaussian normalization, footprint bounds and the moving
// atlas EMA. No floor albedo, surface reconstruction or DLSS can hide a pattern.
function flatPool(scrambled,cap){
  const size=256,count=2048,extent=[11.8/12,13.8/14];
  const covariance=extent.map(e=>.30+.25*(size*e/Math.sqrt(count))**2);
  const scale=Math.min(1,cap/Math.max(...covariance));
  const [xx,yy]=covariance.map(c=>.30+(c-.30)*scale);
  const radius=[3*Math.sqrt(xx),3*Math.sqrt(yy)],history=new Float64Array(size*size);
  let powerError=0;
  for(let frame=0;frame<12;frame++){
    const atlas=new Float64Array(size*size);
    for(let i=0;i<count;i++){
      let uv=sobol(i,frame+3*137);
      if(scrambled)uv=uv.map((u,a)=>scramble(u,hash(frame*2+(a?0x93d7:0x19b2))));
      const center=uv.map((u,a)=>((u-.5)*extent[a]+.5)*size-.5);
      const lo=center.map((c,a)=>Math.max(0,Math.floor(c-radius[a]))),hi=center.map((c,a)=>Math.min(size-1,Math.ceil(c+radius[a])));
      const taps=[];let sum=0;
      for(let y=lo[1];y<=hi[1];y++)for(let x=lo[0];x<=hi[0];x++){
        const r=(x-center[0])**2/xx+(y-center[1])**2/yy;
        if(r<=9){const w=Math.exp(-.5*r);taps.push([y*size+x,w]);sum+=w;}
      }
      for(const [p,w] of taps)atlas[p]+=w/sum;
    }
    powerError=Math.max(powerError,Math.abs(atlas.reduce((a,b)=>a+b,0)/count-1));
    const alpha=1/(Math.min(frame,3)+1);
    for(let p=0;p<atlas.length;p++)history[p]+=(atlas[p]-history[p])*alpha;
  }
  let mean=0,squared=0,n=0;
  for(let y=16;y<size-16;y++)for(let x=16;x<size-16;x++){
    const v=history[y*size+x];mean+=v;squared+=v*v;n++;
  }
  // Strongest coherent Fourier component of a central periodic crop. Random
  // residual estimator noise is acceptable; a repeatable checker is not. The
  // 128-texel crop contains integer periods of the original dyadic pattern.
  const crop=128,frequencies=65;
  const re=new Float64Array(crop*frequencies),im=new Float64Array(crop*frequencies);
  const cos=new Float64Array(crop*frequencies),sin=new Float64Array(crop*frequencies);
  for(let f=0;f<frequencies;f++)for(let x=0;x<crop;x++){
    const phase=2*Math.PI*(f-32)*x/crop;cos[f*crop+x]=Math.cos(phase);sin[f*crop+x]=Math.sin(phase);
  }
  for(let y=0;y<crop;y++)for(let f=0;f<frequencies;f++)for(let x=0;x<crop;x++){
    const v=history[(y+64)*size+x+64];re[y*frequencies+f]+=v*cos[f*crop+x];im[y*frequencies+f]-=v*sin[f*crop+x];
  }
  let peak=0;
  for(let fx=0;fx<frequencies;fx++)for(let fy=0;fy<frequencies;fy++){
    if(fx===32&&fy===32)continue;let r=0,i=0;
    for(let y=0;y<crop;y++){
      const a=re[y*frequencies+fx],b=im[y*frequencies+fx],c=cos[fy*crop+y],s=sin[fy*crop+y];
      r+=a*c+b*s;i+=b*c-a*s;
    }
    peak=Math.max(peak,Math.hypot(r,i)/(crop*crop*(mean/n)));
  }
  return {cv:Math.sqrt(squared/n-(mean/n)**2)/(mean/n),peak,powerError};
}
test('flat room-water caustics do not retain the digital-shift checkerboard',t=>{
  const before=flatPool(false,4),after=flatPool(true,16);
  t.diagnostic(JSON.stringify({before,after}));
  assert.ok(after.powerError<1e-12,'normalized footprints changed radiant power');
  assert.ok(after.cv<.10,`flat pool irradiance CV ${after.cv}`);
  assert.ok(after.peak<before.peak*.4,`insufficient coherent checker reduction: ${JSON.stringify({before,after})}`);
});
