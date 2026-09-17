import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {photonSourceFlux} from './validate-captures.mjs';
const read=p=>readFileSync(new URL(p,import.meta.url),'utf8');

test('foam capture energy audit includes the flashlight and actual environment source selection',()=>{
  const r={fixture:false,waterEnabled:true,waterFloodWatts:140,lasersEnabled:true,laserWattsEach:1.5};
  assert.equal(photonSourceFlux(r),183);
  for(const [environment,watts] of [[0,183],[1,140],[2,280],[3,0]]){
    assert.equal(photonSourceFlux({...r,experience:{environment,flashlight:false}}),watts);
    assert.equal(photonSourceFlux({...r,experience:{environment,flashlight:true}}),watts+8);
  }
  assert.equal(photonSourceFlux({fixture:true,waterEnabled:false,lasersEnabled:false}),40);
});

test('foam markers reconstruct a coating rather than opaque secondary spheres',()=>{
  const update=read('shaders/fluid/whitewater.hlsl'),intersection=read('shaders/fluid/whitewater-intersection.hlsli');
  assert.match(intersection,/uint\(p.previousType.w\)==1\)return/);
  assert.match(update,/if\(uint\(p.previousType.w\)!=1\)/);
  assert.match(update,/void FoamSplat/);assert.match(update,/surfaceAge.w/);
  assert.match(read('src/fluid/whitewater.h'),/sizeof\(Particle\) == 64/);
  const transport=read('shaders/transport.hlsl');
  assert.match(transport,/h.foam=foamCoverage\(h.p,cone/);
  assert.match(transport,/primary.foam\*\(1-F\)\*FoamReflectance/);
  assert.match(transport,/energy\*h.foam/);
  assert.match(read('shaders/lasers.hlsli'),/transmitted=power\*\(1-F\)\*\(1-h.foam\)/);
});
test('foam optical depth blends continuously and fixed point accumulation cannot overflow',()=>{
  const max=8192*.50*65536;assert.ok(max<2**32);
  const coverage=t=>1-Math.exp(-t);
  assert.equal(coverage(0),0);assert.ok(coverage(8)>.999);
  for(const a of [0,.1,.7,2])for(const b of [0,.01,1,2])
    assert.ok(Math.abs(coverage(a+b)-(coverage(a)+(1-coverage(a))*coverage(b)))<1e-14);
  const values=[.001,.4,1.2,2].map(x=>Math.round(x*65536));
  assert.equal(values.reduce((s,v)=>s+v),values.toReversed().reduce((s,v)=>s+v));
  const shader=read('shaders/fluid/foam-field.hlsli');
  assert.match(shader,/1-exp\(-density\)/);assert.doesNotMatch(shader,/random\s*\(|Dimensions.z\*/);
});
test('wet foam reflection, diffuse scattering, transmission and absorption conserve energy',()=>{
  for(const F of [0,.02,.5,1])for(const cover of [0,.1,.7,1])for(const albedo of [.86,.89,.88]){
    const reflected=F,diffuse=(1-F)*cover*albedo,transmitted=(1-F)*(1-cover),absorbed=(1-F)*cover*(1-albedo);
    assert.ok(Math.abs(reflected+diffuse+transmitted+absorbed-1)<1e-14);
  }
});
test('markers feed an advected foam layer, not one visible fuzzy patch each',()=>{
  const splat=read('shaders/fluid/whitewater.hlsl');
  assert.match(splat,/65536\*\.50\*fade\*w\*w\*w/);
  assert.match(splat,/production=3.5\*smoothstep\(\.5,1.8,source\)/);
  assert.match(splat,/void FoamTransport/);
  assert.match(splat,/smoothstep\(\.1,\.65,n.y\)/);
  assert.match(splat,/smoothstep\(0,\.18,p.surfaceAge.w\)/);
  assert.match(splat,/smoothstep\(0,\.65,p.velocityLife.w\)/);
  const field=read('shaders/fluid/foam-field.hlsli');
  assert.match(field,/StructuredBuffer<float4> SurfaceFoam/);
  assert.match(field,/world\+layer.yzw\/density/);
  assert.match(read('src/fluid/whitewater.cpp'),/foamIndex \^= 1/);
});

test('room lattice spans XZ in every complete layer and accounts for particle volume',()=>{
  for(const count of [100000,250000]){
    const extent=[11.83,.305,13.83],volume=extent.reduce((a,b)=>a*b),spacing=Math.cbrt(volume/count);
    const n=[Math.floor(extent[0]/spacing),1,Math.floor(extent[2]/spacing)];
    n[1]=Math.ceil(count/(n[0]*n[2]));const pv=volume/n.reduce((a,b)=>a*b);
    const columns=new Set();let maxY=0;
    for(let id=0;id<count;id++){
      const q=[id%n[0],Math.floor(id/(n[0]*n[2])),Math.floor(id/n[0])%n[2]];
      q.forEach((v,a)=>assert.ok(v>=0&&v<n[a]));columns.add(`${q[0]},${q[2]}`);maxY=Math.max(maxY,q[1]);
    }
    assert.equal(columns.size,n[0]*n[2]);assert.equal(maxY,n[1]-1);
    assert.ok(pv*count<=volume*(1+1e-14));
    assert.ok(volume-pv*count<pv*n[0]*n[2]); // only the final layer may be partial
  }
});
test('inlet integrates SI flux, preserves fractional births and stops at capacity',()=>{
  const pv=.000481184,r=.2,speed=Math.hypot(4.5,.4),rate=Math.PI*r*r*speed/pv;
  let fraction=0,count=100000;const capacity=100123;
  for(const dt of [0,1/120,1/60,1/30,0,1/60,1,1]){
    fraction+=rate*dt;const born=Math.min(capacity-count,Math.floor(fraction));
    count+=born;fraction-=born;assert.ok(count<=capacity);
    assert.ok(Math.abs(born*pv-Math.PI*r*r*(born*pv/(Math.PI*r*r)))<1e-12);
    if(count===capacity)fraction=0;
  }
  assert.equal(count,capacity);assert.equal(fraction,0);
});
test('analytic secondary sphere roots handle entry, exit, tangency and a long camera ray',()=>{
  function roots(q,d,r){const a=d.reduce((s,x)=>s+x*x,0),center=-q.reduce((s,x,i)=>s+x*d[i],0)/a;
    const perp=q.map((x,i)=>x+d[i]*center),disc=(r*r-perp.reduce((s,x)=>s+x*x,0))/a;
    return disc<0?[]:[center-Math.sqrt(disc),center+Math.sqrt(disc)];}
  for(const r of [.008,.021]){
    const far=roots([-50,0,0],[1,0,0],r);assert.ok(Math.abs(far[0]-(50-r))<1e-12);
    assert.deepEqual(roots([0,0,0],[1,0,0],r),[-r,r]);
    assert.deepEqual(roots([-3,r,0],[1,0,0],r),[3,3]);
    assert.deepEqual(roots([-3,r*1.01,0],[1,0,0],r),[]);
  }
});
test('air bubble interfaces invert water IOR and add no water absorption within the cavity',()=>{
  const water=1.333,air=1,normalReflectance=((water-air)/(water+air))**2;
  assert.ok(normalReflectance>.020 && normalReflectance<.021);
  const enter=[water,air],exit=[air,water];assert.equal(enter[0]/enter[1]*exit[0]/exit[1],1);
  const sigma=.2644,waterDistance=2,bubbleDistance=.02;
  assert.ok(Math.abs(Math.exp(-sigma*(waterDistance-bubbleDistance))*Math.exp(-0*bubbleDistance)-
                     Math.exp(-sigma*1.98))<1e-14);
});
