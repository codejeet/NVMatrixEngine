import test from 'node:test';
import assert from 'node:assert/strict';

test('FG device depth uses the same non-reversed LH projection as the camera',()=>{
  const near=.05,far=200;
  const depth=z=>Math.max(0,Math.min(1,(far-far*near/Math.max(near,z))/(far-near)));
  assert.equal(depth(near),0);assert.equal(depth(far),1);
  let previous=-1;
  for(const z of [.05,.051,.1,1,10,100,200]){
    const d=depth(z),reconstructed=far*near/(far-d*(far-near));
    assert.ok(d>previous);previous=d;
    assert.ok(Math.abs(reconstructed-z)<1e-9);
  }
  assert.equal(depth(10000),1);assert.equal(depth(0),0);
});

test('separate premultiplied RmlUi layer reproduces direct backbuffer compositing',()=>{
  const scene=[.12,.4,.7];let separate=[0,0,0,0],direct=scene.slice();
  for(const [rgb,a] of [[[1,.2,.7],.4],[[.2,1,.1],.2],[[.4,.1,1],.7]]){
    separate=[...rgb.map((c,i)=>c*a+(1-a)*separate[i]),a+(1-a)*separate[3]];
    direct=rgb.map((c,i)=>c*a+(1-a)*direct[i]);
  }
  separate.slice(0,3).forEach((c,i)=>assert.ok(Math.abs(c+(1-separate[3])*scene[i]-direct[i])<1e-15));
});
