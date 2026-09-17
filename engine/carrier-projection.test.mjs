import test from 'node:test';
import assert from 'node:assert/strict';

// Exhaustive active-set reference, not the GPU red/black iteration.
function linear(A,b){
  const n=b.length,m=A.map((r,i)=>[...r,b[i]]);
  for(let k=0;k<n;k++){
    let p=k;for(let i=k+1;i<n;i++)if(Math.abs(m[i][k])>Math.abs(m[p][k]))p=i;
    if(Math.abs(m[p][k])<1e-12)return null;
    [m[k],m[p]]=[m[p],m[k]];const d=m[k][k];for(let j=k;j<=n;j++)m[k][j]/=d;
    for(let i=0;i<n;i++)if(i!==k){const d=m[i][k];for(let j=k;j<=n;j++)m[i][j]-=d*m[k][j];}
  }return m.map(r=>r[n]);
}
function extension(cap,q,edges,U=cap.map(()=>1)){
  const n=cap.length,A=Array.from({length:n},()=>Array(n).fill(0)),b=q.map((v,i)=>v-cap[i]*U[i]);
  assert.ok(U.every(v=>v>=0&&v<=1));
  for(const [l,r,Q,w] of edges){
    if(w>0){assert.equal(U[l],1);assert.equal(U[r],1);}
    const phase=Q*U[Q>=0?l:r];b[l]-=phase;b[r]+=phase;
    A[l][l]+=w;A[r][r]+=w;A[l][r]-=w;A[r][l]-=w;
  }
  for(let mask=0;mask<2**n;mask++){
    const active=Array.from({length:n},(_,i)=>i).filter(i=>mask&(1<<i));
    const s=linear(active.map(i=>active.map(j=>A[i][j])),active.map(i=>b[i]));if(!s)continue;
    const p=Array(n).fill(0);active.forEach((i,k)=>p[i]=s[k]);
    const slack=A.map((row,i)=>row.reduce((v,a,j)=>v+a*p[j],-b[i]));
    if(p.some(v=>v< -1e-10)||slack.some(v=>v< -1e-10)||p.some((v,i)=>Math.abs(v*slack[i])>1e-10))continue;
    const result=edges.map(([l,r,Q,w])=>[l,r,Q+w*(p[l]-p[r]),w]);
    for(let i=0;i<edges.length;i++)if(edges[i][3]===0)assert.equal(result[i][2],edges[i][2]);
    const T=cap.map((v,i)=>Array.from({length:n},(_,j)=>i===j?v:0));
    for(const [l,r,Q] of result){const donor=Q>=0?l:r,receiver=Q>=0?r:l;T[donor][donor]+=Math.abs(Q);T[receiver][donor]-=Math.abs(Q);}
    const concentration=linear(T,q);assert.ok(concentration,'Transport needs a connected storage sink');
    const next=concentration.map((v,i)=>v*cap[i]);
    assert.ok(next.every((v,i)=>v>=-1e-10&&v<=cap[i]+1e-10));
    assert.ok(Math.abs(next.reduce((s,v)=>s+v,0)-q.reduce((s,v)=>s+v,0))<1e-10);
    return {p,result,next};
  }
  throw new Error('No feasible air-only extension; liquid pressure/fraction coupling is required');
}
test('air-side extension removes carrier compression without changing the liquid interface flux',()=>{
  const r=extension([1,1,1],[1,1,0],[[0,1,1,0],[1,2,0,1]]);
  assert.deepEqual(r.result.map(e=>e[2]),[1,1]);assert.deepEqual(r.next,[.5,.75,.75]);
});
test('already feasible carrier flow is unchanged',()=>{
  const edges=[[0,1,.1,0],[1,2,.1,1]],r=extension([1,1,1],[.5,.5,0],edges);
  assert.deepEqual(r.result,edges);assert.deepEqual(r.p,[0,0,0]);
});
test('extension respects unequal conductances and shares correction conservatively',()=>{
  const r=extension([1,1,1,1],[1,1,0,0],[[0,1,.8,0],[1,2,0,1],[1,3,0,3]]);
  assert.ok(Math.abs(r.result[1][2]-.2)<1e-12);assert.ok(Math.abs(r.result[2][2]-.6)<1e-12);
});
test('a physically projected closing cell can evacuate without an air correction',()=>{
  const r=extension([0,1,1],[.2,.5,0],[[0,1,1,0],[1,2,0,1]]);
  assert.equal(r.next[0],0);assert.equal(r.result[0][2],1);
});
test('an isolated capacity violation is rejected, not hidden by clipping inventory',()=>{
  assert.throws(()=>extension([1,1],[1,1],[[0,1,1,0]]),/No feasible air-only/);
});
test('a donor upper envelope avoids the false assumption that every incoming carrier is pure liquid',()=>{
  const cap=[1,1,1,1],q=[.5,.9,.9,0],edges=[[0,1,.5,0],[1,2,0,1]];
  // The fixed first donor has concentration .5/(1+.5). Treating it as 1
  // overestimates the incoming water and makes the closed air component infeasible.
  assert.throws(()=>extension(cap,q,edges),/No feasible air-only/);
  const r=extension(cap,q,edges,[1/3,1,1,0]);
  assert.ok(Math.abs(r.result[1][2]-1/15)<1e-12);
  assert.ok(r.next.every((v,i)=>v<=cap[i]+1e-12));
});

// Nonlinear reference uses scalar bisection (the GPU uses finite piecewise
// Newton steps). Acceptance independently solves the resulting linear upwind
// transport matrix, rather than accepting the trial fractions as inventory.
function coupled(cap,source,edges) {
  const n=cap.length,c=Array(n).fill(0),p=Array(n).fill(0);
  const local=(i)=>edges.flatMap(([l,r,Q,w])=>l===i?[[Q-w*p[r],w,c[r]]]:r===i?[[-Q-w*p[l],w,c[l]]]:[]);
  let residual=Infinity;
  for(let it=0;it<2048;it++) {
    for(let i=0;i<n;i++) {
      const e=local(i),d=e.reduce((v,[Q])=>v+Math.max(0,Q),cap[i]);
      const b=e.reduce((v,[Q,,c])=>v-Math.min(0,Q)*c,source[i]);
      if(b<=d) {p[i]=0;c[i]=d>0?b/d:0;continue;}
      c[i]=1;
      const F=x=>e.reduce((v,[Q,w,c])=>{const q=Q+w*x;return v+q*(q>=0?1:c);},cap[i]-source[i]);
      if(!e.some(([,w])=>w>0)){p[i]=0;continue;}
      let lo=0,hi=1;while(F(hi)<0&&hi<1e16)hi*=2;
      assert.ok(F(hi)>=0,'No local capacity sink');
      for(let k=0;k<70;k++){const mid=(lo+hi)/2;if(F(mid)<0)lo=mid;else hi=mid;}
      p[i]=(lo+hi)/2;
    }
    residual=Math.max(...cap.map((v,i)=>Math.abs(local(i).reduce((r,[Q,w,cn])=>{
      const q=Q+w*p[i];return r+q*(q>=0?c[i]:cn);
    },v*c[i]-source[i]))));
    if(residual<1e-11)break;
  }
  assert.ok(residual<1e-11,`Nonlinear mass residual ${residual}`);
  const flows=edges.map(([l,r,Q,w])=>[l,r,Q+w*(p[l]-p[r]),w]);
  const T=cap.map((v,i)=>Array.from({length:n},(_,j)=>i===j?v:0));
  for(const [l,r,Q] of flows){const d=Q>=0?l:r,rn=Q>=0?r:l;T[d][d]+=Math.abs(Q);T[rn][d]-=Math.abs(Q);}
  const exact=linear(T,source);assert.ok(exact);
  assert.ok(exact.every((v,i)=>v>=-1e-9&&v<=1+1e-9&&Math.abs(v-c[i])<1e-9));
  assert.ok(Math.abs(exact.reduce((s,v,i)=>s+v*cap[i],0)-source.reduce((s,v)=>s+v,0))<1e-9);
  assert.ok(p.every((v,i)=>v>=0&&(v===0||c[i]===1)));
  edges.forEach((e,i)=>{if(e[3]===0)assert.equal(flows[i][2],e[2]);});
  return {flows,p,c:exact};
}
test('coupled carrier leaves a valid mixed-phase region unchanged despite an infeasible unit certificate',()=>{
  const edges=[[0,1,.5,0],[1,2,0,1]],r=coupled([1,1,1,1],[.5,.7,.9,0],edges);
  assert.throws(()=>extension([1,1,1,1],[.5,.7,.9,0],edges),/No feasible air-only/);
  assert.deepEqual(r.flows,edges);assert.deepEqual(r.p,[0,0,0,0]);
});
test('coupled carrier activates only to keep actual transported water inside capacity',()=>{
  const r=coupled([1,1,1],[1,1,0],[[0,1,1,0],[1,2,0,1]]);
  assert.ok(Math.abs(r.flows[1][2]-.5)<1e-10);
  assert.ok(Math.abs(r.c[0]-.5)<1e-10&&Math.abs(r.c[1]-1)<1e-10&&Math.abs(r.c[2]-.5)<1e-10);
});
test('coupled correction selects the new upwind donor when a compressive air flow reverses',()=>{
  const r=coupled([1,1,1],[1,1,0],[[0,1,1,0],[1,2,-.25,1]]);
  assert.ok(r.flows[1][2]>0);assert.ok(Math.abs(r.flows[1][2]-.5)<1e-10);
});
test('coupled transport accounts for a disappearing capacity without retaining donor residue',()=>{
  const r=coupled([0,1,1],[.2,.5,0],[[0,1,1,0],[1,2,0,1]]);
  assert.ok(Math.abs(r.c[1]-.7)<1e-10);assert.deepEqual(r.p,[0,0,0]);
});
test('coupled transport rejects a real protected-face overfill instead of clipping inventory',()=>{
  assert.throws(()=>coupled([1,1],[1,1],[[0,1,1,0]]),/Nonlinear mass residual/);
});
test('eight piecewise Newton steps agree with independent bisection across six flow-reversal kinks',()=>{
  let seed=63;const random=()=>((seed=Math.imul(seed,1664525)+1013904223>>>0)/2**32);
  for(let sample=0;sample<1000;sample++) {
    const e=Array.from({length:6},()=>[(random()-.5)*20,10**(random()*6-3),random()<.2?0:random()]);
    const capacity=random(),atZero=e.reduce((s,[q,,c])=>s+q*(q>=0?1:c),capacity);
    const source=Math.max(0,atZero)+.001+random();
    const F=p=>e.reduce((s,[q,w,c])=>{const v=q+w*p;return s+v*(v>=0?1:c);},capacity-source);
    let p=Math.max(0,...e.map(([q,w])=>-q/w))-F(0)/e.reduce((s,[,w])=>s+w,0);
    assert.ok(F(p)>-1e-9);
    let lo=0,hi=p;
    for(let k=0;k<80;k++){const m=(lo+hi)/2;if(F(m)<0)lo=m;else hi=m;}
    const expected=(lo+hi)/2;
    for(let k=0;k<8;k++) {
      const slope=e.reduce((s,[q,w,c])=>s+w*(q+w*p>=0?1:c),0);
      if(slope>0)p=Math.max(0,p-F(p)/slope);
    }
    assert.ok(Math.abs(p-expected)<1e-9*(1+expected));
    assert.ok(Math.abs(F(p))<1e-9*(1+source));
  }
});
test('a bounded continuation can require interface flux changes while preserving liquid divergence',()=>{
  // Cell 0 is a solved liquid pressure row. Air-only extension has no DOF:
  // every edge touches it. A paired interface correction preserves its exact
  // divergence yet routes liquid away from the overfilled receiving cell.
  const q=[1,.9,0,.5],edges=[[3,0,1],[0,1,1],[0,2,0]];
  const solve=flows=>{
    const T=Array.from({length:4},(_,i)=>Array.from({length:4},(_,j)=>+(i===j)));
    for(const [l,r,Q] of flows){T[l][l]+=Q;T[r][l]-=Q;}
    return linear(T,q);
  };
  const before=solve(edges);assert.ok(before[1]>1);
  const afterEdges=[[3,0,1],[0,1,.16],[0,2,.84]],after=solve(afterEdges);
  assert.equal(edges[1][2]+edges[2][2]-edges[0][2],0);
  assert.equal(afterEdges[1][2]+afterEdges[2][2]-afterEdges[0][2],0);
  assert.ok(after.every(v=>v>=0&&v<=1+1e-12));
  assert.ok(Math.abs(after.reduce((s,v)=>s+v,0)-q.reduce((s,v)=>s+v,0))<1e-12);
  // This is an integration requirement, not a claim that the air-only GPU
  // kernel implements the divergence-preserving coupled pressure correction.
});
test('capacity pressure targets a conservative residual without granting capacity to closed cells',()=>{
  const margin=(capacity,scale)=>Math.min(.1*5e-7*scale,1e-8*capacity);
  const tau=margin(.004,.008),w=.003,deficit=1e-8;
  const p=(deficit+tau)/w;
  assert.ok(w*p-deficit>=0);
  assert.ok(Math.abs((w*p-deficit)-tau)<1e-20);
  assert.equal(margin(0,1),0);
  assert.ok(margin(1,1000)<=1e-8);
  assert.ok(margin(1000,1)<=.1*5e-7);
});
test('capacity coupling removes the physical pressure residual instead of freezing it',()=>{
  // A full cell with a tiny inward pressure residual: a divergence-free delta
  // cannot stop overfill. The correct target is divergence(Q + deltaQ) = 0.
  const capacity=1,source=1,dt=1/120,residual=-3e-5;
  const Q=dt*residual;
  assert.ok(source/(capacity+Q)>1);
  const deltaQ=-Q;
  assert.equal(Q+deltaQ,0);
  assert.equal(source/(capacity+Q+deltaQ),1);
  // A moving wall contributes its endpoint volume change to that equation.
  const swept=-.1,canonical=.0999999;
  const correction=-(canonical+swept);
  assert.ok(Math.abs(canonical+correction+swept)<1e-15);
});
test('holding capacity pressure still refreshes donor fractions when the MAC flux changes',()=>{
  const source=[.4,.2],oldQ=.08,newQ=.1;
  const old=linear([[1+oldQ,0],[-oldQ,1]],source);
  assert.ok(Math.abs((1+newQ)*old[0]-source[0])>1e-3);
  const updated=[source[0]/(1+newQ),0];
  updated[1]=source[1]+newQ*updated[0];
  const reference=linear([[1+newQ,0],[-newQ,1]],source);
  updated.forEach((c,i)=>assert.ok(Math.abs(c-reference[i])<1e-14));
  assert.ok(Math.abs(updated[0]+updated[1]-source[0]-source[1])<1e-14);
});
test('fully liquid parent capacity pressure is a redundant gauge, not a new flux degree of freedom',()=>{
  const lambda=[.3,-.2,.7],p=[.5,.1],parent=[0,0,1],liquid=[true,true,false];
  const total=i=>(liquid[i]?lambda[i]:0)+p[parent[i]];
  const before=[[0,1],[1,2]].map(([l,r])=>total(l)-total(r));
  // Every open child of parent 0 is liquid: absorb its p into the liquid
  // multipliers. Parent 1 contains air, so its capacity pressure stays active.
  for(let i=0;i<3;i++)if(parent[i]===0)lambda[i]+=p[0];
  p[0]=0;
  const after=[[0,1],[1,2]].map(([l,r])=>total(l)-total(r));
  before.forEach((v,i)=>assert.ok(Math.abs(v-after[i])<1e-14));
});
test('air-only capacity coordinates preserve the complete mixed liquid/air pressure field',()=>{
  const liquid=[true,false,true,false],parent=[0,0,1,1],p=[.5,.1],lambda=[.3,0,.7,0];
  const original=liquid.map((wet,i)=>(wet?lambda[i]:0)+p[parent[i]]);
  const totalLiquid=liquid.map((wet,i)=>wet?lambda[i]+p[parent[i]]:0);
  const changed=liquid.map((wet,i)=>wet?totalLiquid[i]:p[parent[i]]);
  assert.deepEqual(changed,original);
  for(const [l,r,w] of [[0,1,2],[1,2,3],[2,3,4]]) {
    const wl=liquid[l]?0:w,wr=liquid[r]?0:w;
    const harmonic=w*(totalLiquid[l]-totalLiquid[r]);
    const flux=harmonic+wl*p[parent[l]]-wr*p[parent[r]];
    assert.ok(Math.abs(flux-w*(original[l]-original[r]))<1e-14);
  }
});
test('internal air-liquid faces stabilize a capacity unknown with no direct exterior-air face',()=>{
  // Exact coupled pressure matrix [2 -1; -1 1]. Eliminating the liquid row
  // yields S=1/2 for air pressure. The direct exterior-air coefficient is zero,
  // but the internal-face energy diagonal is 1 and must not be omitted.
  const demand=.1,airEnergy=1;let p=0,lambda=0,increment=0;
  for(let i=0;i<50;i++) {
    lambda=p/2;
    const residual=demand-(p-lambda);
    increment=residual/airEnergy;
    p=Math.max(0,p+increment);
  }
  lambda=p/2;
  assert.ok(Math.abs(p-.2)<1e-14);
  assert.ok(Math.abs(2*lambda-p)<1e-14);
  assert.ok(Math.abs((p-lambda)-demand)<1e-14);
  assert.ok(Math.abs(airEnergy*increment)<1e-14,'No surviving stabilizer flux at the fixed point');
});
test('a signed upper-solution certificate catches overfill hidden by a small unsigned residual',()=>{
  const flow=4e-7,source=[.5*(1+flow),1];
  const T=[[1+flow,0],[-flow,1]],trial=[.5,1];
  const residual=T.map((row,i)=>row.reduce((s,w,j)=>s+w*trial[j],-source[i]));
  assert.ok(Math.max(...residual.map(Math.abs))<5e-7);
  assert.ok(Math.min(...residual)<-1e-12);
  const unbounded=linear(T,source);assert.ok(unbounded[1]>1);
  const cap=[1,1,1],q=[.4,1,0],upper=[.37,1,.05];
  const boundedMatrix=[[1.1,0,0],[-.1,1.05,0],[0,-.05,1]];
  const certificate=boundedMatrix.map((row,i)=>row.reduce((s,w,j)=>s+w*upper[j],-q[i]));
  assert.ok(certificate.every(r=>r>=0));
  const actual=linear(boundedMatrix,q);
  assert.ok(actual.every((c,i)=>c>=0&&c<=upper[i]&&upper[i]<=cap[i]));
  assert.ok(Math.abs(actual.reduce((s,c)=>s+c,0)-q.reduce((s,c)=>s+c,0))<1e-14);
});
