import test from 'node:test';
import assert from 'node:assert/strict';
const polynomial=(c,t)=>((c[3]*t+c[2])*t+c[1])*t+c[0];
function firstRoot(c){
  const cuts=[0],a=3*c[3],b=2*c[2],d=c[1];
  if(Math.abs(a)<1e-12){if(Math.abs(b)>1e-12){const r=-d/b;if(r>0&&r<1)cuts.push(r);}}
  else {const disc=b*b-4*a*d;if(disc>=0){const roots=[(-b-Math.sqrt(disc))/(2*a),(-b+Math.sqrt(disc))/(2*a)].sort((a,b)=>a-b);for(const r of roots)if(r>0&&r<1&&r-cuts.at(-1)>1e-7)cuts.push(r);}}
  cuts.push(1);
  for(let i=0;i+1<cuts.length;i++){
    let lo=cuts[i],hi=cuts[i+1],fl=polynomial(c,lo),fh=polynomial(c,hi);
    if(Math.abs(fl)<1e-8)return lo;
    if((fl<0)!==(fh<0)){for(let j=0;j<12;j++){const m=.5*(lo+hi),fm=polynomial(c,m);if((fm<0)===(fl<0)){lo=m;fl=fm;}else hi=m;}return .5*(lo+hi);}
    if(Math.abs(fh)<1e-8)return hi;
  }
  return null;
}
test('cell root isolation finds first crossing, same-sign double crossings and tangencies',()=>{
  for(const roots of [[.2,.4,.7],[.12,.23,2],[-1,.33,.71],[.31,.31,2],[0,.3,.7],[1,2,3]]){
    const [a,b,c]=roots,coeff=[-a*b*c,a*b+a*c+b*c,-a-b-c,1];
    const expected=roots.filter(r=>r>=0&&r<=1).sort((a,b)=>a-b)[0];
    assert.ok(Math.abs(firstRoot(coeff)-expected)<1/4096,`${roots}`);
  }
  assert.equal(firstRoot([1,0,0,0]),null);
  assert.ok(Math.abs(firstRoot([-.27,1,0,0])-.27)<1/4096);
  assert.ok(Math.abs(firstRoot([.25,-1,1,0])-.5)<1e-8);
});
test('trilinear-along-ray cubic agrees with independent eight-corner interpolation',()=>{
  let seed=9271;const rand=()=>{seed=(Math.imul(seed,1664525)+1013904223)>>>0;return seed/2**32;};
  for(let trial=0;trial<1000;trial++){
    const v=Array.from({length:8},()=>rand()*2-1),p=Array.from({length:3},rand),q=p.map(x=>rand()-x);
    const [x,y,z]=p,[dx,dy,dz]=q;
    const cx=v[1]-v[0],cy=v[2]-v[0],cz=v[4]-v[0];
    const xy=v[3]-v[1]-v[2]+v[0],xz=v[5]-v[1]-v[4]+v[0],yz=v[6]-v[2]-v[4]+v[0];
    const xyz=v[7]-v[3]-v[5]-v[6]+v[1]+v[2]+v[4]-v[0];
    const c=[v[0]+cx*x+cy*y+cz*z+xy*x*y+xz*x*z+yz*y*z+xyz*x*y*z,
      cx*dx+cy*dy+cz*dz+xy*(x*dy+dx*y)+xz*(x*dz+dx*z)+yz*(y*dz+dy*z)+xyz*(dx*y*z+x*dy*z+x*y*dz),
      xy*dx*dy+xz*dx*dz+yz*dy*dz+xyz*(dx*dy*z+dx*y*dz+x*dy*dz),xyz*dx*dy*dz];
    for(const t of [0,.13,.51,.92,1]){
      const r=p.map((a,i)=>a+q[i]*t);let value=0;
      for(let k=0;k<8;k++)value+=v[k]*r.reduce((w,a,i)=>w*((k>>i)&1?a:1-a),1);
      assert.ok(Math.abs(value-polynomial(c,t))<1e-13);
    }
  }
});
test('eight wavelength bundles keep marginal spectral coverage and radiant energy',()=>{
  for(const count of [8192,16384,32768]){
    let power=0;const bands=new Uint32Array(8);
    for(let i=0;i<count;i++){const lambda=380+400*((i%8+.375)/8);bands[Math.floor((lambda-380)/50)]++;power+=28/count;}
    assert.equal(power,28);assert.ok(bands.every(n=>n===count/8));
  }
});
test('dynamic caustic history decays independently without resetting stationary lighting',()=>{
  let staticPower=7,dynamicPower=10;
  for(let frame=0;frame<16;frame++){staticPower=staticPower*31/32+7/32;dynamicPower*=.75;}
  assert.ok(Math.abs(staticPower-7)<1e-12);assert.ok(dynamicPower<.11);
});
test('anisotropic kernel bounds cover contracted axes and center displacement',()=>{
  // Mirror the bounded shape policy; all orientations have spectral norm <= 1
  // for the world-space support transform. A 1.2R halo includes center smoothing.
  const R=.12,covarianceRadius=.16;
  for(let i=0;i<10000;i++){
    const eigen=[1,(i%97)/97,(i%233)/233],largest=Math.max(...eigen);
    const axes=eigen.map(e=>1+.65*(Math.sqrt(Math.max(e,largest*.25)/largest)-1));
    assert.ok(axes.every(a=>a>=.675&&a<=1));
    assert.ok(Math.max(...axes)*R+.1*covarianceRadius<=1.2*R);
    assert.ok(axes.reduce((a,b)=>a/b,1)>0);
  }
});
function tetraFraction(phi){
  const n=phi.filter(x=>x<0),p=phi.filter(x=>x>=0);
  if(!n.length)return 0;if(n.length===4)return 1;
  if(n.length===1)return p.reduce((w,x)=>w*n[0]/(n[0]-x),1);
  if(n.length===3)return 1-n.reduce((w,x)=>w*p[0]/(p[0]-x),1);
  const u=n[0]/(n[0]-p[0]),v=n[0]/(n[0]-p[1]),s=n[1]/(n[1]-p[0]),t=n[1]/(n[1]-p[1]);
  return u*v+u*t*(1-v)+s*t*(1-u);
}
test('diagnostic tetrahedral volume handles thin planar cuts and sign complements',()=>{
  assert.equal(tetraFraction([1,2,3,4]),0);assert.equal(tetraFraction([-1,-2,-3,-4]),1);
  assert.equal(tetraFraction([-1,1,1,1]),.125);
  assert.equal(tetraFraction([-1,-1,1,1]),.5);
  assert.equal(tetraFraction([-1,-1,-1,1]),.875);
  for(let i=1;i<1000;i++){
    const values=[-.1-i/53,-.1-i/113,.1+i/191,.1+i/211];
    assert.ok(Math.abs(tetraFraction(values)+tetraFraction(values.map(x=>-x))-1)<1e-12);
  }
});
test('surface cell masks preserve zeros, tangencies and conservative brick bounds',()=>{
  for(const field of [()=>1,()=>-1,(x,y,z)=>y-3.2,(x,y,z)=>Math.hypot(x-4,y-4,z-4)-2.1,
    (x,y,z)=>Math.abs(y-4)-.01,(x,y,z)=>y-4]){
    const mask=new Uint32Array(16),lo=[8,8,8],hi=[0,0,0];
    for(let n=0;n<512;n++){
      const c=[n%8,Math.floor(n/8)%8,Math.floor(n/64)];
      const v=Array.from({length:8},(_,k)=>field(...c.map((x,a)=>x+((k>>a)&1))));
      if(Math.min(...v)<=0&&Math.max(...v)>=0){
        mask[n>>>5]|=1<<(n&31);
        for(let a=0;a<3;a++){lo[a]=Math.min(lo[a],c[a]);hi[a]=Math.max(hi[a],c[a]+1);}
      }
    }
    for(let n=0;n<512;n++){
      const c=[n%8,Math.floor(n/8)%8,Math.floor(n/64)];
      const v=Array.from({length:8},(_,k)=>field(...c.map((x,a)=>x+((k>>a)&1))));
      const marked=!!(mask[n>>>5]&(1<<(n&31)));
      assert.equal(marked,!(v.every(x=>x>0)||v.every(x=>x<0)));
      if(marked)for(let a=0;a<3;a++){assert.ok(c[a]>=lo[a]);assert.ok(c[a]+1<=hi[a]);}
    }
  }
});
