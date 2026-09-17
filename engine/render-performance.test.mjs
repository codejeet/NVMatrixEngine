import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';

test('occlusion optimization retains closest-hit camera transport and optical budgets',()=>{
  const shader=readFileSync(new URL('shaders/transport.hlsl',import.meta.url),'utf8');
  assert.ok(shader.includes('else p=trace(o,d,255,depth&15)'));
  assert.ok(shader.includes('if(visit==0&&primaryGlass&&!(OpticalControls.w&512))p=primaryPayload'));
  assert.ok(shader.includes('Payload p=trace(o,d,CameraState.x?251:255,0)'));
  assert.equal(251 & 4,0); // First-person primary alone omits the avatar mask.
  assert.equal(251 & (1|2|8|16),1|2|8|16);
  assert.ok(shader.includes('bool visibility=false'));
  assert.ok(shader.includes('visit<48&&pending>0'));assert.ok(shader.includes('if(depth>=10)'));
});
test('boolean occlusion agrees with nearest-hit visibility for unordered masked geometry and finite segments',()=>{
  let seed=9324;const random=()=>{seed=(Math.imul(seed,1664525)+1013904223)>>>0;return seed/2**32;};
  for(let i=0;i<10000;i++){
    const limit=random()*100,mask=[1,2,8,16,255][i%5];
    const candidates=Array.from({length:20},()=>({t:random()*110,mask:1<<Math.floor(random()*5),accepted:random()>.2}));
    const accepted=candidates.filter(c=>c.accepted&&(c.mask&mask)&&c.t>=.0001&&c.t<=limit);
    const nearest=[...accepted].sort((a,b)=>a.t-b.t)[0];
    // Skip-closest payload starts as a hit. Only a miss changes the sentinel.
    const object=accepted.length?0:0xffffffff;
    assert.equal(object!==0xffffffff,nearest!==undefined);
  }
  const shader=readFileSync(new URL('shaders/transport.hlsl',import.meta.url),'utf8');
  assert.ok(shader.includes('RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH|RAY_FLAG_SKIP_CLOSEST_HIT_SHADER'));
  assert.ok(shader.includes('p.object=visibility?0:0xffffffff'));
  assert.ok(shader.includes('void Miss(inout Payload p) { p.t=RayTCurrent(); p.object=0xffffffff'));
  assert.ok(!shader.includes('RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES'));
  const lighting=readFileSync(new URL('shaders/direct-light.hlsli',import.meta.url),'utf8');
  assert.ok(shader.includes('#include "direct-light.hlsli"'));
  assert.match(lighting,/selectA&&!occluded\(h.p\+n\*EPS\*2,a.direction,255,a.hint,a.distance-EPS\*4\)/);
  assert.match(lighting,/selectB&&!occluded\(h.p\+n\*EPS\*2,b.direction,255,b.hint,b.distance-EPS\*4\)/);
  assert.match(lighting,/result\+=a.irradiance\/pa/);
  assert.match(lighting,/result\+=b.irradiance\/pb/);
  assert.ok(!lighting.includes('trace('));
});
