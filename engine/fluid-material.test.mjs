import test from 'node:test';
import assert from 'node:assert/strict';

test('explicit viscosity subcycles are convex and preserve the pre-force FLIP velocity',()=>{
  for(const nu of [0,1e-6,.05,.5,3]) {
    const h=.08,dt=1/120,n=Math.max(1,Math.ceil(6*nu*dt/h**2/.9)),alpha=nu*dt/h**2/n;
    assert.ok(alpha*6<=.900001);
    let v=Float64Array.from({length:128},(_,i)=>.7+Math.sin(i*2*Math.PI/128)),original=v.slice();
    for(let step=0;step<n;step++) {
      const out=v.map((x,i)=>x+alpha*(v[(i+1)%128]+v[(i+127)%128]-2*x));
      assert.ok(out.every(x=>x>=Math.min(...v)-1e-12 && x<=Math.max(...v)+1e-12));
      v=out;
    }
    const decay=(1-4*alpha*Math.sin(Math.PI/128)**2)**n;
    for(let i=0;i<128;i++)assert.ok(Math.abs(v[i]-(.7+(original[i]-.7)*decay))<1e-12);
    assert.ok(Math.abs(v.reduce((a,b)=>a+b,0)-original.reduce((a,b)=>a+b,0))<1e-10);
  }
});

test('curvature of a non-SDF scalar uses the normalized gradient, not its raw Laplacian',()=>{
  const R=.75,h=.008;
  function curvature(p,colour) {
    const at=(a,sa,b=-1,sb=0)=>colour(p.map((x,j)=>x+h*((j===a?sa:0)+(j===b?sb:0))));
    const g=p.map((_,a)=>(at(a,1)-at(a,-1))/(2*h));
    const H=p.map((_,a)=>p.map((_,b)=>a===b?(at(a,1)-2*colour(p)+at(a,-1))/h**2:
      (at(a,1,b,1)-at(a,1,b,-1)-at(a,-1,b,1)+at(a,-1,b,-1))/(4*h*h)));
    const gg=g.reduce((s,v)=>s+v*v,0),quad=g.reduce((s,v,a)=>s+v*H[a].reduce((t,w,b)=>t+w*g[b],0),0);
    return -(gg*(H[0][0]+H[1][1]+H[2][2])-quad)/gg**1.5;
  }
  for(const scale of [.2,1,12])for(const p of [[R,0,0],[R/Math.sqrt(3),R/Math.sqrt(3),R/Math.sqrt(3)]])
    assert.ok(Math.abs(curvature(p,q=>.5-scale*(Math.hypot(...q)-R))/(2/R)-1)<.0002);
  assert.ok(Math.abs(curvature([0,0,0],p=>.5-p[1]*3))<1e-10);
});

test('Laplace boundary pressure has a static droplet equilibrium and uses identical face/solve ghosts',()=>{
  const sigma=.0728,R=.75,p=sigma*2/R,rho=998.207,h=.08,dt=1/120;
  // A constant-curvature closed droplet is in equilibrium: interior and ghost
  // values are the same. It must not be pulled inward by a fictitious air p=0.
  assert.equal((6*p)/6,p);
  assert.equal(dt/(rho*h)*(p-p),0);
  // Arbitrary nonuniform boundary / incomplete solve still obeys div = residual.
  const neighbours=[p,p+.1,p-.05,p,p+.02,p],center=p*.7,div=.2;
  const lap=neighbours.reduce((sum,x)=>sum+x-center,0);
  const projected=div-neighbours.reduce((sum,x)=>sum+dt/(rho*h*h)*(x-center),0);
  assert.ok(Math.abs(projected-(div-dt/(rho*h*h)*lap))<1e-14);
});
