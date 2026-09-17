import test from 'node:test';
import assert from 'node:assert/strict';

const lattice=()=>Array.from({length:64},(_,i)=>({
  x:[i%4,Math.floor(i/4)%4,Math.floor(i/16)].map(v=>(v+.5)*.5-1),
  v:[.5,0,.003], mass:.125, C:[0,0,0,0,0,0,0,0,0],
}));
function moments(particles){
  const M=particles.reduce((s,p)=>s+p.mass,0);
  const mean=[0,1,2].map(a=>particles.reduce((s,p)=>s+p.mass*p.v[a],0)/M);
  const center=[0,1,2].map(a=>particles.reduce((s,p)=>s+p.mass*p.x[a],0)/M);
  const covariance=Array.from({length:9},(_,i)=>particles.reduce((s,p)=>s+p.mass*
    (p.x[Math.floor(i/3)]-center[Math.floor(i/3)])*(p.x[i%3]-center[i%3]),0)/M);
  const unresolved=particles.reduce((s,p)=>s+p.mass*(p.v.reduce((r,v,a)=>r+(v-mean[a])**2,0)+
    .25*p.C.reduce((r,c)=>r+c*c,0)),0)/M;
  return {M,mean,center,covariance,unresolved};
}
test('flowing admission uses comoving velocity error, not an absolute wake-speed cap',()=>{
  const samples=lattice();samples[0].v[1]=.001;
  const before=moments(samples);
  for(const p of samples)p.v=p.v.map((v,a)=>v+[32,-16,8][a]);
  const after=moments(samples);
  assert.ok(Math.abs(before.unresolved-after.unresolved)<1e-17);
  assert.deepEqual(before.center,after.center);
  assert.deepEqual(before.covariance,after.covariance);
});
test('a mean-only owner must reject unresolved APIC rotation even with identical particle velocities',()=>{
  const samples=lattice();for(const p of samples){p.C[1]=-.2;p.C[3]=.2;}
  const q=moments(samples);
  assert.ok(Math.abs(q.unresolved-.02)<1e-14);
  assert.ok(Math.sqrt(q.unresolved)>.025);
});
test('coarse quadrature has an explicit spatial error, not a claimed exact lattice reconstruction',()=>{
  const samples=lattice(), baseline=moments(samples);
  baseline.covariance.forEach((v,i)=>assert.equal(v,i%4===0?5/16:0));
  for(let i=0;i<64;i++)samples[i].x[0]+=(i%2?-.005:.005);
  const deformed=moments(samples);
  assert.ok(Math.abs(deformed.covariance[0]-.310025)<1e-14);
  assert.ok(Math.abs(deformed.covariance[0]-baseline.covariance[0])<.035);
  for(const p of samples)p.x[1]+=.12;
  assert.ok(Math.abs(moments(samples).center[1]-.12)<1e-14);
  assert.ok(moments(samples).center[1]>.025);
});
test('joint fine-cell coverage apportions only the grid-owned quantity, never its particle replica',()=>{
  const gridVolume=4,particleVolume=4,open=Array(8).fill(1),coarse=open.reduce((a,b)=>a+b,0);
  const coverage=open.map(v=>gridVolume*v/coarse+particleVolume/8);
  assert.equal(coverage.reduce((a,b)=>a+b,0),gridVolume+particleVolume);
  assert.ok(coverage.every(v=>v===1));
});
