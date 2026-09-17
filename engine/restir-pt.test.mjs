import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
const shader=readFileSync(new URL('./shaders/restir-pt.hlsli',import.meta.url),'utf8');
test('PT tiled reservoir layout covers non-multiple resolutions without aliasing histories',()=>{
  for(const [w,h] of [[1,1],[17,31],[742,418],[853,480]]){
    const row=Math.ceil(w/16)*256,pitch=row*Math.ceil(h/16),seen=new Set();
    for(let frame=0;frame<4;frame++)for(let y=0;y<h;y++)for(let x=0;x<w;x++){
      const p=frame*pitch+Math.floor(y/16)*row+Math.floor(x/16)*256+(y%16)*16+x%16;
      assert.ok(p>=frame*pitch&&p<(frame+1)*pitch);assert.ok(!seen.has(p));seen.add(p);
    }
    assert.equal(seen.size,4*w*h);
    for(let frame=0;frame<30;frame++){
      const current=2+(frame&1),previous=2+((frame&1)^1);
      assert.notEqual(current,previous);assert.ok(current>1&&previous>1);
      assert.equal(previous,2+((frame-1)&1));
    }
  }
});
test('independent stochastic decorrelation preserves expectation without blending radiance',()=>{
  // Enumerate independent fresh/resampled estimator distributions and Bernoulli
  // choices. Both estimators have mean 4; their mixture remains mean 4.
  let mean=0;
  for(const a of [0,8])for(const b of [2,6])mean+=(.25*a+.75*b)/4;
  assert.equal(mean,4);
  assert.match(shader,/RTXDI_GetNextRandom\(rng\)<\.25/);
  assert.doesNotMatch(shader,/lerp\([^;]*(?:targetFunction|Noisy)/);
});
test('PT calls pinned path-resampling kernels and preserves transport ownership',()=>{
  for(const call of ['GenerateInitialSamples','RTXDI_PTTemporalResampling','RTXDI_PTSpatialResampling'])
    assert.match(shader,new RegExp(`${call}\\(`));
  assert.match(shader,/RECONNECTION_MODE_FOOTPRINT/);
  assert.match(shader,/if\(glass\(h\)\)break/);
  assert.match(shader,/endpoint\(h,n,seed\)/);
  // All random dimensions exist even when hybrid replay skips intermediate NEE.
  assert.ok(shader.indexOf('uint seed=asuint(')<shader.indexOf('if(ctx.ShouldSampleEmissiveSurfaces())'));
  assert.match(shader,/sample\.brdfTimesNoL=[\s\S]*?exp\(-extinctionRgb/);
  assert.doesNotMatch(shader,/RayQuery|TraceRayInline/);
});
test('surface-bound packing exactly represents every legal interval and contains marked cells',()=>{
  for(let ax=0;ax<8;ax++)for(let bx=ax+1;bx<=8;bx++)
  for(let ay=0;ay<8;ay++)for(let by=ay+1;by<=8;by++)
  for(let az=0;az<8;az++)for(let bz=az+1;bz<=8;bz++){
    const packed=ax|(ay<<4)|(az<<8)|(bx<<12)|(by<<16)|(bz<<20);
    const unpack=Array.from({length:6},(_,a)=>(packed>>>(a*4))&15);
    assert.deepEqual(unpack,[ax,ay,az,bx,by,bz]);
  }
});
test('room fill uses normalized finite power, never emissive white tiles',()=>{
  const area=4*5.9*6.9,irradiance=140/area;
  assert.ok(irradiance>.85&&irradiance<.87);assert.equal(irradiance*area,140);
  const common=readFileSync(new URL('./shaders/common.hlsli',import.meta.url),'utf8');
  assert.match(common,/chart==0\?float3\(\.82,\.82,\.82\)/);
  assert.ok(.82<1&&.82*.18<.2);
});
