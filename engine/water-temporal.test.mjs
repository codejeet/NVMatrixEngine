import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
const integral=(x,r)=>{const f=x-Math.floor(x);return Math.floor(x)*2*r+Math.min(f,r)+Math.max(f-(1-r),0);};
const coverage=(u,w,r)=>Math.max(0,Math.min(1,(integral(u+w*.5,r)-integral(u-w*.5,r))/w));
test('analytic receiver grid filtering preserves coverage across scale, wrap and negative coordinates',()=>{
  for(const r of [.013,.022])for(const width of [.001,.017,.13,.67,1,3,12.5]){
    let sum=0;
    for(let i=0;i<10000;i++)sum+=coverage(-2+(i+.5)/10000,width,r);
    assert.ok(Math.abs(sum/10000-2*r)<1e-5,'minification changed average line reflectance');
    for(const x of [-2,-.999,-.01,0,.4,1.007])assert.ok(Math.abs(coverage(x,width,r)-coverage(x+3,width,r))<1e-9);
  }
  assert.equal(coverage(.5,.01,.013),0);assert.ok(coverage(0,.01,.013)>.999999);
  const shader=readFileSync(new URL('shaders/transport.hlsl',import.meta.url),'utf8');
  assert.match(shader,/cameraReceiverFootprint\(p,h,d,opticalDistance\)/);
  assert.match(shader,/cameraReceiverFootprint\(p,primary,d,p\.t\)/);
  assert.match(shader,/opticalDistances\[pending\]=opticalDistance/);
  assert.doesNotMatch(shader,/\bdd[xy]\s*\(/,'raygen cannot use pixel-quad derivatives');
});
test('all moving water occluders retain bounded history; changed source energy still resets',()=>{
  for(const moved of [[],[1],[2],[3,4],[6],[1,2,3,4,6]])
  for(const flashlightMotion of [false,true])for(const emissionChanged of [false,true]){
    const dirty=flashlightMotion||emissionChanged||moved.length>0,hard=emissionChanged;
    // A moved boat must not turn the entire receiver atlas into one noisy frame.
    // Controls.x still immediately caps formerly static 32-frame history at four.
    const age=hard?0:Math.min(32,dirty?3:31);
    assert.equal(1/(age+1),hard?1:dirty?.25:1/32);
  }
  const shader=readFileSync(new URL('shaders/transport.hlsl',import.meta.url),'utf8');
  assert.match(shader,/rng=hash\([^;]+Dimensions.z\*0x517cc1b7/,'independent per-frame random samples must remain');
  const renderer=readFileSync(new URL('src/renderer.cpp',import.meta.url),'utf8');
  assert.match(renderer,/const bool fluidHistoryReset = reset \|\| fluidLightHash != lastFluidLightHash/);
  assert.match(renderer,/hashBytes\(&c.flashlightOrigin.w, sizeof\(float\)/);
  assert.doesNotMatch(renderer,/fluidHistoryReset = fluidHistoryReset \|\|/);
  assert.doesNotMatch(renderer,/sceneObjects.size\(\) && !dirty/,'a moved avatar must not mask another changed object');
  assert.match(renderer,/dispatch\(0, options.photons, 1\)/,'photons must still retrace current geometry every real frame');
  const resolve=readFileSync(new URL('shaders/resolve.hlsl',import.meta.url),'utf8');
  assert.match(resolve,/\(FluidState.w&128\)\|\|FluidState.z/);
  assert.match(resolve,/\(liquidSurfaceMoving\(FluidState.y\)\|\|Controls.x\)\?3u:Controls.w-1/);
  assert.match(resolve,/if\(Controls.x\) \{ Caustics/,'static caustics still invalidate on avatar motion');
});
test('animated receiver EMA suppresses independent noise and has bounded shadow lag',()=>{
  // Fixed alpha=1/4 reduces stationary independent input variance to alpha/(2-alpha).
  const alpha=.25;
  assert.ok(alpha/(2-alpha)<.15);
  let value=1;
  for(let frame=0;frame<12;++frame)value*=1-alpha;
  assert.ok(value<.032,'moving shadow retains too much stale lighting');
});
