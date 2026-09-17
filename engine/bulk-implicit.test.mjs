import test from 'node:test';
import assert from 'node:assert/strict';

// Independent dense elimination, not a replay of the planned GPU Jacobi solve.
function solve(capacity,edges,source){
  const n=capacity.length,A=Array.from({length:n},(_,i)=>Array.from({length:n},(_,j)=>i===j?capacity[i]:0));
  for(const [from,to,Q] of edges){A[from][from]+=Q;A[to][from]-=Q;}
  const augmented=A.map((row,i)=>[...row,...source[i]]);
  for(let k=0;k<n;k++){
    let pivot=k;for(let i=k+1;i<n;i++)if(Math.abs(augmented[i][k])>Math.abs(augmented[pivot][k]))pivot=i;
    assert.ok(Math.abs(augmented[pivot][k])>1e-14,'Singular closed transport component');
    [augmented[k],augmented[pivot]]=[augmented[pivot],augmented[k]];
    const scale=augmented[k][k];for(let j=k;j<n+4;j++)augmented[k][j]/=scale;
    for(let i=0;i<n;i++)if(i!==k){const s=augmented[i][k];for(let j=k;j<n+4;j++)augmented[i][j]-=s*augmented[k][j];}
  }
  const c=augmented.map(row=>row.slice(n)),next=c.map((q,i)=>q.map(v=>v*capacity[i]));
  for(let i=0;i<n;i++)for(let a=0;a<4;a++)assert.ok(Math.abs(A[i].reduce((s,v,j)=>s+v*c[j][a],0)-source[i][a])<1e-10);
  for(let a=0;a<4;a++)assert.ok(Math.abs(next.reduce((s,q)=>s+q[a],0)-source.reduce((s,q)=>s+q[a],0))<1e-10);
  assert.ok(next.every(q=>q[3]>=-1e-12));
  const energy=qs=>qs.reduce((sum,q)=>sum+(q[3]>0?q.slice(0,3).reduce((s,v)=>s+v*v,0)/(2*q[3]):0),0);
  assert.ok(energy(next)<=energy(source)+1e-10);
  return {c,next};
}
test('a closing donor passes all mass and momentum onward without a 95 percent cap',()=>{
  const {c,next}=solve([0,2],[[0,1,1]],[[3,-2,1,1],[0,1,-1,1]]);
  assert.ok(next[0].every(v=>v===0));assert.deepEqual(next[1],[3,-1,0,2]);
  assert.deepEqual(c.map(q=>q[3]),[1,1]);
});
test('implicit transport crosses a chain of disappearing cells in the same substep',()=>{
  const {next}=solve([0,0,3],[[0,1,1],[1,2,2]],[[2,0,0,1],[-1,0,0,1],[0,0,0,1]]);
  assert.deepEqual(next.map(q=>q[3]),[0,0,3]);assert.equal(next[2][0],1);
});
test('constant fractions and velocities survive GCL-compatible changing capacities',()=>{
  const old=[1,1,1],cap=[.2,.9,1.9],edges=[[0,1,.8],[1,2,.9]];
  for(const fraction of [0,.01,.4,1]){
    const {c}=solve(cap,edges,old.map(v=>[2*v*fraction,-v*fraction,3*v*fraction,v*fraction]));
    for(const q of c)for(const [a,v] of [2,-1,3,1].entries())assert.ok(Math.abs(q[a]-v*fraction)<1e-12);
  }
});
test('high-CFL periodic mixing is positive, conservative and dissipative',()=>{
  const n=17,edges=Array.from({length:n},(_,i)=>[i,(i+1)%n,50]);
  const {next}=solve(Array(n).fill(1),edges,Array.from({length:n},(_,i)=>i===0?[4,-3,2,1]:[0,0,0,0]));
  assert.ok(next.every(q=>q[3]>0&&q[3]<=1));
});
test('positivity alone does not establish a free-surface upper bound',()=>{
  const {next}=solve([1,.1],[[0,1,1]],[[0,0,0,1],[0,0,0,.1]]);
  assert.ok(next[1][3]>.1,'Incompatible volume flow must remain visible as overfill');
});
test('an entirely closed trapped component is rejected rather than erasing water',()=>{
  assert.throws(()=>solve([0,0],[[0,1,1],[1,0,1]],[[0,0,0,1],[0,0,0,1]]),/Singular/);
});
