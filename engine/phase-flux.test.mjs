import test from 'node:test';
import assert from 'node:assert/strict';

// Graph-level FP64 reference, independent of HLSL indexing/dispatch. Candidate
// transfers have already passed the existing donor-positivity/CFL constraint.
function limit(q,capacity,edges,maxIterations=128) {
  const f=edges.map(e=>e.transfer.slice());let iterations=0;
  const evaluate=()=>{
    const incoming=q.map(()=>0),outgoing=q.map(()=>0);
    edges.forEach((e,k)=>{const w=f[k][3];incoming[w>=0?e.r:e.l]+=Math.abs(w);outgoing[w>=0?e.l:e.r]+=Math.abs(w);});
    return q.map((v,i)=>{
      const room=Math.max(capacity[i],v[3])-v[3]+outgoing[i];
      return incoming[i]>room+Math.max(1e-12,capacity[i]*2e-7)?Math.max(0,room)/incoming[i]*(1-2e-7):1;
    });
  };
  let scales=evaluate();
  for(;iterations<maxIterations&&scales.some(s=>s<1);++iterations){
    edges.forEach((e,k)=>{const r=f[k][3]>=0?e.r:e.l;f[k]=f[k].map(v=>v*scales[r]);});
    scales=evaluate();
  }
  const next=q.map(v=>v.slice());
  edges.forEach((e,k)=>f[k].forEach((v,a)=>{next[e.l][a]-=v;next[e.r][a]+=v;}));
  return {next,flux:f,iterations,exhausted:scales.some(s=>s<1)};
}
const state=(volume,velocity=[.3,-.2,1.7])=>[...velocity.map(v=>v*volume),volume];
const sum=q=>q.reduce((s,v)=>s.map((x,i)=>x+v[i]),[0,0,0,0]);
const energy=q=>q.reduce((s,v)=>s+(v[3]>0?v.slice(0,3).reduce((a,b)=>a+b*b,0)/v[3]/2:0),0);
const close=(a,b,tolerance=1e-10)=>assert.ok(Math.abs(a-b)<=tolerance,`${a} != ${b}`);
function audit(q,cap,result){
  assert.ok(!result.exhausted);
  result.next.forEach((v,i)=>{assert.ok(v.every(Number.isFinite));assert.ok(v[3]>=-1e-14);assert.ok(v[3]<=Math.max(cap[i],q[i][3])+Math.max(1e-11,cap[i]*4e-7));});
  sum(result.next).forEach((v,i)=>close(v,sum(q)[i]));
  assert.ok(energy(result.next)<=energy(q)+1e-10);
}
test('competing receivers limit shared transfers, conserving all momentum components',()=>{
  const q=[state(1,[2,3,-4]),state(1,[-1,0,2]),state(.9)],cap=[1,1,1];
  const edges=[{l:0,r:2,transfer:q[0].map(v=>v*.8)},{l:1,r:2,transfer:q[1].map(v=>v*.8)}];
  const r=limit(q,cap,edges);audit(q,cap,r);assert.equal(r.iterations,1);close(r.next[2][3],1,1e-7);
  assert.ok(r.flux.every((f,k)=>f.every((v,a)=>Math.abs(v-edges[k].transfer[a]*r.flux[k][3]/edges[k].transfer[3])<1e-12)));
});
test('full-cell circulation is untouched, unlike a receiver-free-space-only clamp',()=>{
  const cap=[1,1,1,1],q=cap.map(v=>state(v));
  const edges=q.map((v,i)=>({l:i,r:(i+1)%4,transfer:v.map(x=>x*.7)}));
  const r=limit(q,cap,edges);audit(q,cap,r);assert.equal(r.iterations,0);assert.deepEqual(r.flux,edges.map(e=>e.transfer));
  r.next.forEach((v,i)=>v.forEach((x,a)=>close(x,q[i][a])));
});
test('backpressure propagates upstream without duplicating or dropping liquid',()=>{
  const cap=Array(12).fill(1),q=cap.map(v=>state(v)),edges=q.slice(1).map((_,i)=>({l:i,r:i+1,transfer:state(.4)}));
  const r=limit(q,cap,edges);audit(q,cap,r);assert.equal(r.iterations,11);
  assert.ok(r.flux.every(v=>v[3]===0));
});
test('a finite convergence budget is reported, never mislabeled a bounded update',()=>{
  const cap=Array(12).fill(1),q=cap.map(v=>state(v)),edges=q.slice(1).map((_,i)=>({l:i,r:i+1,transfer:state(.4)}));
  const r=limit(q,cap,edges,2);assert.ok(r.exhausted);assert.ok(r.next.some(v=>v[3]>1.1));
});
test('closing-cell capacity loss remains explicit and is not repaired by clipping',()=>{
  const q=[state(1),state(.1)],cap=[0,1],r=limit(q,cap,[]);audit(q,cap,r);
  assert.deepEqual(r.next,q);assert.equal(r.next[0][3]-cap[0],1);
});
test('GCL-compatible full/partial liquid transport needs no receiver correction',()=>{
  const old=[.19,.32,.08,.27],cap=old.slice(),volumes=[.003,-.001,.002,-.004];
  const edges=volumes.map((v,i)=>({l:i,r:(i+1)%4,transfer:state(v)}));
  edges.forEach((e,i)=>{cap[e.l]-=volumes[i];cap[e.r]+=volumes[i];});
  for(const fraction of [.01,.5,1]){
    const q=old.map(v=>state(v*fraction));const e=edges.map(v=>({...v,transfer:v.transfer.map(x=>x*fraction)}));
    const r=limit(q,cap,e);audit(q,cap,r);assert.equal(r.iterations,0);
    r.next.forEach((v,i)=>close(v[3],cap[i]*fraction));
  }
});
test('tiny volumes, negative oriented flux, and zero flux remain finite and accounted',()=>{
  const q=[state(1e-10,[-1,4,.1]),state(1e-10)],cap=[1e-10,1e-10];
  const edges=[{l:1,r:0,transfer:state(-.95e-10,[-1,4,.1])}];
  const r=limit(q,cap,edges);audit(q,cap,r);
  assert.ok(r.flux[0][3]===0);assert.deepEqual(r.next,q);
});
test('random competing directed transfers stay positive, bounded and dissipative',()=>{
  let seed=2971;const rand=()=>((seed=Math.imul(seed,1664525)+1013904223>>>0)/2**32);
  for(let trial=0;trial<100;++trial){
    const cap=Array.from({length:24},()=>.1+rand()),q=cap.map(v=>state(v*rand(),[rand()*4-2,rand()*4-2,rand()*4-2]));
    const edges=[];
    for(let i=0;i<q.length;++i)for(let j=0;j<3;++j){const target=(i+1+j)%q.length;
      const factor=rand()*.3;
      edges.push({l:i,r:target,transfer:q[i].map(v=>v*factor)});
    }
    audit(q,cap,limit(q,cap,edges));
  }
});
