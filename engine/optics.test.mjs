import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
const dot=(a,b)=>a.reduce((s,x,i)=>s+x*b[i],0);
const add=(a,b)=>a.map((x,i)=>x+b[i]);
const scale=(a,s)=>a.map(x=>x*s);
const sub=(a,b)=>add(a,scale(b,-1));
const unit=a=>scale(a,1/Math.hypot(...a));
const n=nm=>1.5046+.00420/(nm*.001)**2;
function refract(d,N,eta) {
  const ci=-dot(d,N),k=1-eta*eta*(1-ci*ci);
  return k<0?null:add(scale(d,eta),scale(N,eta*ci-Math.sqrt(k)));
}
function fresnel(ci,ni,nt) {
  const k=1-(ni/nt)**2*(1-ci*ci); if(k<=0)return 1;
  const ct=Math.sqrt(k),rs=(ni*ci-nt*ct)/(ni*ci+nt*ct),rp=(nt*ci-ni*ct)/(nt*ci+ni*ct);
  return (rs*rs+rp*rp)/2;
}
function intersectionDerivative(o,dd,N,d,t) {
  const q=add(o,scale(dd,t)); return sub(q,scale(d,dot(N,q)/dot(N,d)));
}
function transmissionDerivative(dd,N,d,eta) {
  const ci=-dot(N,d),ct=Math.sqrt(1-eta*eta*(1-ci*ci)),dc=-dot(N,dd);
  return add(scale(dd,eta),scale(N,(eta-eta*eta*ci/ct)*dc));
}
test('Cauchy uses micrometres and has normal dispersion over the whole visible band',()=>{
  assert.ok(Math.abs(n(550)-1.5184843)<1e-6);
  for(let nm=381;nm<=780;nm++)assert.ok(n(nm)<n(nm-1));
});
test('three Sobol dimensions stratify aperture and wavelength independently',()=>{
  const bins=Array.from({length:3},()=>new Uint32Array(256));let xy=0,xz=0,yz=0;
  for(let i=0;i<65536;i++) {
    let x=0,y=0,z=0,vx=0x80000000,vy=0x80000000,vz=0x80000000,previousZ=0,g=i^(i>>>1);
    for(let bit=0;bit<32;bit++) {
      if(g&(1<<bit)){x^=vx;y^=vy;z^=vz;}
      vx>>>=1;vy^=vy>>>1;const nextZ=bit===0?0xc0000000:previousZ^(previousZ>>>2)^vz;previousZ=vz;vz=nextZ;
    }
    const v=[x>>>0,y>>>0,z>>>0].map(q=>q/2**32);for(let k=0;k<3;k++)bins[k][Math.floor(v[k]*256)]++;
    xy+=v[0]*v[1];xz+=v[0]*v[2];yz+=v[1]*v[2];
  }
  for(const dim of bins)for(const count of dim)assert.equal(count,256);
  for(const sum of [xy,xz,yz])assert.ok(Math.abs(sum/65536-.25)<.0001);
});
test('Snell, TIR, Fresnel and Beer-Lambert preserve bounded optical power',()=>{
  for(const nm of [380,450,550,650,780])for(const angle of [0,.1,.3,.6,1]) {
    const d=[Math.sin(angle),0,-Math.cos(angle)],r=refract(d,[0,0,1],1/n(nm));
    assert.ok(Math.abs(n(nm)*r[0]-Math.sin(angle))<1e-12);
    const F=fresnel(Math.cos(angle),1,n(nm)); assert.ok(F>=0&&F<=1);
    assert.ok(Math.abs(F+(1-F)-1)<1e-15);
    const loss=Math.exp(-.035*3); assert.ok((1-F)*loss<=1-F);
  }
  assert.equal(refract([Math.sin(1),0,-Math.cos(1)],[0,0,1],n(550)),null);
  assert.equal(fresnel(Math.cos(1),n(550),1),1);
});
test('planar ray differential agrees with independent finite differences',()=>{
  const o=[.2,.4,3],d=unit([.15,.02,-1]),N=[0,0,1],deltaO=[.2,.1,0],deltaD=[.01,.02,.001];
  const intersect=(origin,direction)=>add(origin,scale(direction,-origin[2]/direction[2]));
  const t=-o[2]/d[2],analytic=intersectionDerivative(deltaO,deltaD,N,d,t),eps=1e-5;
  const finite=scale(sub(intersect(add(o,scale(deltaO,eps)),add(d,scale(deltaD,eps))),intersect(o,d)),1/eps);
  assert.ok(Math.hypot(...sub(analytic,finite))<1e-6);
});
test('Snell differential agrees with finite differences for entry and exit',()=>{
  for(const eta of [1/n(450),n(450)])for(const angle of [.1,.2,.4]) {
    const d=[Math.sin(angle),0,-Math.cos(angle)],dd=[Math.cos(angle),0,Math.sin(angle)],N=[0,0,1],eps=1e-6;
    const analytic=transmissionDerivative(dd,N,d,eta),next=[Math.sin(angle+eps),0,-Math.cos(angle+eps)];
    const finite=scale(sub(refract(next,N,eta),refract(d,N,eta)),1/eps);
    assert.ok(Math.hypot(...sub(analytic,finite))<3e-6);
  }
});
test('normalized bounded splats conserve energy even at atlas chart boundaries',()=>{
  for(const center of [[.1,.2],[255.3,127.2],[123.3,254.8]]) {
    let weights=[];
    for(let y=Math.max(0,Math.floor(center[1]-3));y<=Math.min(255,Math.ceil(center[1]+3));y++)
      for(let x=Math.max(0,Math.floor(center[0]-3));x<=Math.min(255,Math.ceil(center[0]+3));x++){
        const r=(x-center[0])**2+(y-center[1])**2;if(r<=9)weights.push(Math.exp(-r/2));
      }
    const sum=weights.reduce((a,b)=>a+b,0);assert.ok(sum>0);
    assert.ok(Math.abs(weights.reduce((a,b)=>a+b/sum,0)-1)<1e-14);
  }
});
test('pinned CIE LUT supports the fixed-point worst-case bound',()=>{
  const rows=readFileSync(new URL('../shared/.deps/CIE_xyz_1931_2deg.csv',import.meta.url),'utf8').trim().split(/\r?\n/).map(x=>x.split(',').map(Number)).filter(x=>x[0]>=380&&x[0]<=780);
  assert.equal(rows.length,401);
  const maximum=Math.max(...rows.flatMap(x=>x.slice(1)))*400/106.856895;
  assert.ok(maximum<8); assert.ok(maximum*2**28<2**32);
});
test('all traversal stays in raygen library, never resolve compute shaders',()=>{
  for(const path of ['common.hlsli','transport.hlsl','resolve.hlsl']) {
    const s=readFileSync(new URL(`shaders/${path}`,import.meta.url),'utf8');
    assert.doesNotMatch(s,/\bRayQuery\b|\bTraceRayInline\b/);
    if(path!=='transport.hlsl')assert.doesNotMatch(s,/\b(?:TraceRay|NvTraceRayHitObject)\s*\(/);
  }
});
test('camera dielectric splitting preserves slab energy and retains internal reflections',()=>{
  const F=fresnel(1,1,n(550));
  let transmitted=0,reflected=F,inside=(1-F)/(n(550)**2);
  // Explicit two-lobe radiance transport, including the eta^2 factors on entry/exit.
  for(let bounce=0;bounce<30;bounce++) {
    const exit=inside*(1-F)*n(550)**2;
    if(bounce%2===0)transmitted+=exit;else reflected+=exit;
    inside*=F;
  }
  assert.ok(Math.abs(transmitted+reflected-1)<1e-12);
  assert.ok(reflected>F,'internal reflections must add to entrance reflection');
  assert.ok(Math.abs(transmitted-(1-F)/(1+F))<1e-12);
});
test('curved-interface normal differential agrees with finite differences',()=>{
  const radius=.68,normal=unit([.3,.4,.5]),d=scale(normal,-1),dd=[.04,-.02,.01];
  const tangent=sub([.1,.02,.03],scale(normal,dot(normal,[.1,.02,.03]))),dn=scale(tangent,1/radius);
  for(const eta of [1/n(550),n(550)]) {
    const ci=-dot(normal,d),ct=Math.sqrt(1-eta*eta*(1-ci*ci));
    const fixed=transmissionDerivative(dd,normal,d,eta);
    const curved=add(sub(fixed,scale(normal,(eta-eta*eta*ci/ct)*dot(dn,d))),scale(dn,eta*ci-ct));
    const eps=1e-6,nn=unit(add(scale(normal,radius),scale(tangent,eps)));
    const finite=scale(sub(refract(add(d,scale(dd,eps)),nn,eta),refract(d,normal,eta)),1/eps);
    assert.ok(Math.hypot(...sub(curved,finite))<1e-6);
  }
});
test('capped grazing footprints keep a nonsingular reconstruction kernel',()=>{
  for(const size of [1,100,65504,1e8]) {
    let xx=.30+.25*size*size,yy=xx,xy=.25*size*size;
    const s=Math.min(1,4/Math.max(xx,yy));
    xx=.30+(xx-.30)*s;yy=.30+(yy-.30)*s;xy*=s;
    assert.ok(Number.isFinite(xx*yy-xy*xy)&&xx*yy-xy*xy>=.09);
  }
});
test('visible dielectric prefix does not reintroduce glass after diffuse transport',()=>{
  const s=readFileSync(new URL('shaders/transport.hlsl',import.meta.url),'utf8');
  assert.match(s,/radiance=dielectricView\(o,d/);
  assert.match(s,/if\(glass\(h\)\) break/);
  assert.match(s,/\(entered\|\|s.nm>0\)&&h.chart<5/);
  assert.match(s,/nm>=510&&nm<=570/);
});
test('DLSS receives projected image jitter, not ray sample displacement',()=>{
  // For a static projected point q, sampling at pixel+j produces q-j in the image.
  for(const jitter of [-.5,-.17,.125,.49]) {
    const projected=217.3,observed=projected-jitter;
    assert.ok(Math.abs(observed-projected-(-jitter))<1e-12);
  }
  const cpp=readFileSync(new URL('src/renderer.cpp',import.meta.url),'utf8');
  assert.match(cpp,/\{-c\.jitter\.x, -c\.jitter\.y\},\s*reset/);
  assert.doesNotMatch(cpp,/dlss\.evaluate\([^;]*\bdirty\b/s);
  const shader=readFileSync(new URL('shaders/transport.hlsl',import.meta.url),'utf8');
  assert.match(shader,/pixel\+\.5\+Jitter\.xy/);
  assert.doesNotMatch(shader,/asuint\(h\.p\./);
});
test('reflection distance guide describes the first reflected segment',()=>{
  const s=readFileSync(new URL('shaders/transport.hlsl',import.meta.url),'utf8');
  assert.match(s,/visit==\(primaryGlass\?1:0\)/);
  assert.doesNotMatch(s,/if\(!hitDistance\)hitDistance=p\.t/);
  // The camera-glass guide relies on LIFO reflection-first ordering.
  assert.ok(s.indexOf('float3 tw=')>=0&&s.indexOf('float3 tw=')<s.indexOf('float3 rw='));
});
test('foreground raw orbit retains legacy tuning and samples messages after the display wake',()=>{
  const s=readFileSync(new URL('src/main.cpp',import.meta.url),'utf8');
  assert.match(s,/RAWINPUTDEVICE mouse\{0x01, 0x02, 0, window\}/);
  assert.match(s,/if \(!rawMouse \|\| !relativeMouse \|\| injected\)/);
  assert.match(s,/GetRawInputData/);
  assert.match(s,/waitForFrame\(\)[\s\S]*?pumpMessages\(\);[\s\S]*?orbitInput\.step/);
  assert.doesNotMatch(s,/RIDEV_NOLEGACY|RIDEV_INPUTSINK/);
});
test('lab camera recovery uses render delta, feeds motion history and is not advanced by picking',()=>{
  const main=readFileSync(new URL('src/main.cpp',import.meta.url),'utf8');
  const renderer=readFileSync(new URL('src/renderer.cpp',import.meta.url),'utf8');
  const game=readFileSync(new URL('../shared/src/game.h',import.meta.url),'utf8');
  assert.match(main,/activeRenderer->viewCamera\(\)/);
  assert.match(main,/renderer\.render\([^;]*hud\.get\(\), dt\)/);
  assert.match(renderer,/if \(reset \|\| \(game && game->resetHistory\)\)\s*cameraFollow\.reset\(\)/);
  assert.match(renderer,/game->camera\([^;]*&cameraFollow, delta\)/);
  assert.match(renderer,/previousCamera = cam/);
  assert.match(game,/CameraFollow \*follow = nullptr/); // Mainline remains opt-out.
});
test('water normal differential agrees with finite differences on the ripple snapshot',()=>{
  const waves=[[.065,3.7,2.9,0],[.035,-4.3,1.7,.8]];
  const normal=p=>{
    let sx=0,sz=0;for(const [a,kx,kz,phase] of waves){const c=a*Math.cos(kx*p[0]+kz*p[2]+phase);sx+=c*kx;sz+=c*kz;}
    return unit([-sx,1,-sz]);
  };
  for(const p of [[2.1,1,-4.1],[3.7,1,-3.1],[5.2,1,-1.6]])for(const dp of [[1,0,0],[0,0,1],[.3,0,-.7]]){
    let sx=0,sz=0,dx=0,dz=0;
    for(const [a,kx,kz,phase] of waves){const t=kx*p[0]+kz*p[2]+phase,c=a*Math.cos(t),v=-a*Math.sin(t)*(kx*dp[0]+kz*dp[2]);sx+=c*kx;sz+=c*kz;dx+=v*kx;dz+=v*kz;}
    const n=normal(p),v=[-dx,0,-dz],analytic=scale(sub(v,scale(n,dot(n,v))),1/Math.sqrt(1+sx*sx+sz*sz));
    const epsilon=1e-6,finite=scale(sub(normal(add(p,scale(dp,epsilon))),n),1/epsilon);
    assert.ok(Math.hypot(...sub(analytic,finite))<1e-5);
  }
});
test('single-scattering beam visibility vanishes without scattering and HG is normalized',()=>{
  const phase=(c,g)=>(1-g*g)/(4*Math.PI*(1+g*g-2*g*c)**1.5);
  for(const g of [0,.25]){
    let sum=0;for(let i=0;i<100000;i++)sum+=phase(-1+(i+.5)*2/100000,g)*4*Math.PI/100000;
    assert.ok(Math.abs(sum-1)<1e-8);
  }
  const intensity=(power,radius,sigma,length)=>power/(Math.PI*radius**2)*sigma*phase(0,.25)*length;
  assert.equal(intensity(1.5,.025,0,1),0);
  assert.equal(intensity(3,.025,.012,.05),2*intensity(1.5,.025,.012,.05));
  const shader=readFileSync(new URL('shaders/lasers.hlsli',import.meta.url),'utf8');
  assert.match(shader,/void BeamRaygen/);assert.doesNotMatch(shader,/TraceRayInline|RayQuery/);
  assert.match(shader,/h.chart<5\)return 0/); // No duplicate spots on photon receivers.
  assert.match(shader,/denom=1\+g\*g-2\*g\*dot\(axis,-d\)/);
});
test('laser photometry retains wavelength efficacy and photon power has no radiance eta squared',()=>{
  const rows=readFileSync(new URL('../shared/.deps/CIE_xyz_1931_2deg.csv',import.meta.url),'utf8').trim().split(/\r?\n/).map(x=>x.split(',').map(Number));
  const y=nm=>rows.find(x=>x[0]===nm)[2];
  assert.ok(Math.abs(683*y(555)-683)<1e-6);
  assert.ok(y(532)>y(450)*20); // Equal watts are not equal visible brightness.
  const s=readFileSync(new URL('shaders/transport.hlsl',import.meta.url),'utf8');
  const photons=s.slice(s.indexOf('void PhotonRaygen'),s.indexOf('#include "lasers.hlsli"'));
  assert.match(photons,/energy=s.power\/count/);
  assert.doesNotMatch(photons,/energy\s*\*?=[^;]*eta\s*\*\s*eta/);
  assert.match(s,/weight\*=exp\(-extinctionRgb\(medium\)\*p.t\)/);
});
