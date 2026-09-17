import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
const clamp=x=>Math.min(1,Math.max(0,x));
const smooth=(a,b,x)=>{const t=clamp((x-a)/(b-a));return t*t*(3-2*t);};
const hash=s=>{s=(s^(s>>>16))>>>0;s=Math.imul(s,0x7feb352d);s^=s>>>15;s=Math.imul(s,0x846ca68b);return (s^(s>>>16))>>>0;};
const noiseValue=(x,y,seed)=>(hash(x^hash(y+seed))>>>8)*(2/16777216)-1;
const quintic=x=>x*x*x*(x*(x*6-15)+10);
const lerp=(a,b,t)=>a+(b-a)*t;
function noise(x,y,seed){
  const cx=Math.floor(x),cy=Math.floor(y),u=quintic(x-cx),v=quintic(y-cy);
  return lerp(lerp(noiseValue(cx,cy,seed),noiseValue(cx+1,cy,seed),u),
              lerp(noiseValue(cx,cy+1,seed),noiseValue(cx+1,cy+1,seed),u),v);
}
function grain(x,y,footprint=0){
  const scales=[.071,.023,.0083,.0031],weights=[.15,.50,.25,.10];
  [x,y]=[.932327*x-.361615*y,.361615*x+.932327*y];let value=0;
  for(let i=0;i<4;i++){
    const resolved=1-smooth(.2,.85,footprint/scales[i]);
    if(resolved>0)value+=weights[i]*resolved*noise(x/scales[i],y/scales[i],8927+i*1013);
    [x,y]=[.8*x-.6*y,.6*x+.8*y];
  }
  return value;
}
const advance=(old,source,dt)=>{
  const decay=Math.exp(-dt/1.4),production=3.5*smooth(.5,1.8,source);
  return Math.min(4,old*decay+production*2*1.4*(1-decay));
};
test('a lone marker cannot grow visible foam; entrainment clusters can',()=>{
  for(const dt of [0,1/180,1/120,1/60,.1])for(const source of [0,.01,.25,.5])
    assert.equal(advance(0,source,dt),0);
  assert.ok(advance(0,1,1/60)>0);
  let density=0;for(let i=0;i<600;i++)density=advance(density,100,1/60);
  assert.equal(density,4);
});
test('foam production/decay is timestep-consistent and pause is exact',()=>{
  for(const start of [0,.3,2,4])for(const source of [0,.5,.8,1.5,10]){
    const whole=advance(start,source,1/30);
    assert.ok(Math.abs(whole-advance(advance(start,source,1/60),source,1/60))<1e-12);
    assert.equal(advance(start,source,0),start);
  }
});
test('material coordinates advect with the layer instead of swimming through it',()=>{
  const world=[1,2,3],velocity=[.6,.05,-.2],dt=1/60,displacement=[.2,0,-.1];
  const nextWorld=world.map((x,i)=>x+velocity[i]*dt);
  const nextDisplacement=displacement.map((x,i)=>x-velocity[i]*dt);
  for(let i=0;i<3;i++)assert.ok(Math.abs(world[i]+displacement[i]-nextWorld[i]-nextDisplacement[i])<1e-14);
  const density=.4,weighted=displacement.map(x=>x*density);
  weighted.forEach((x,i)=>assert.ok(Math.abs(x/density-displacement[i])<1e-14));
});
test('foam grain has no lattice seams and is fixed in material space',()=>{
  for(let x=-10;x<10;x++)for(let i=0;i<11;i++){
    const y=i*.19;
    assert.ok(Math.abs(noise(x-1e-7,y,8927)-noise(x+1e-7,y,8927))<1e-5);
    assert.equal(grain(x+.123,y),grain(x+.123,y));
  }
});
test('multiscale grain filters to its zero mean without repeating a tile',()=>{
  const footprints=[0,.001,.01,.1],sums=footprints.map(()=>0),squares=footprints.map(()=>0),samples=16000;
  let tileDifference=0;
  for(let i=0;i<samples;i++){
    const x=hash(i+18721)/2**32*32,y=hash(i+76541)/2**32*32;
    footprints.forEach((f,k)=>{const g=grain(x,y,f);assert.ok(g>=-1&&g<=1);sums[k]+=g;squares[k]+=g*g;});
    tileDifference+=Math.abs(grain(x,y)-grain(x+.071,y));
  }
  sums.forEach(sum=>assert.ok(Math.abs(sum/samples)<.015));
  assert.equal(squares[3],0);assert.ok(squares[0]>squares[1]&&squares[1]>squares[2]);
  const scales=[.071,.023,.0083,.0031],weights=[.15,.50,.25,.10];
  const expectedRatio=weights.reduce((s,w,k)=>s+w*w*(1-smooth(.2,.85,.01/scales[k]))**2,0)/
                      weights.reduce((s,w)=>s+w*w,0);
  assert.ok(Math.abs(squares[2]/squares[0]-expectedRatio)<.025);
  assert.ok(tileDifference/samples>.1);
  const field=readFileSync(new URL('shaders/fluid/foam-field.hlsli',import.meta.url),'utf8');
  assert.match(field,/scales\[4\]=\{.071,.023,.0083,.0031\}/);
  assert.match(field,/1-smoothstep\(\.2,\.85,footprint\/scales\[band\]\)/);
  assert.doesNotMatch(field,/foamCellWall|foamCellHash|cellMean|second-nearest|Dimensions.z|random\(/);
});
test('grain preserves mean opacity and cannot produce opaque dots or hard cutout holes',()=>{
  for(let i=0;i<=40;i++){
    const density=i/10,width=.055+.125*(density/4);
    const previousScatter=width*(2.30-1.5*width);
    const scatter=.1219625+density*(.06671875-.00146484375*density);
    assert.ok(Math.abs(scatter-previousScatter)<1e-14);
    const wet=lerp(.04,.35,smooth(1.5,4,density)),mean=lerp(wet,1,scatter);
    const amplitude=.85*Math.min(mean,1-mean);
    for(const g of [-1,-.5,0,.5,1])assert.ok(mean+amplitude*g>0&&mean+amplitude*g<1);
    assert.ok(Math.abs((mean-amplitude+mean+amplitude)/2-mean)<1e-14);
  }
});
