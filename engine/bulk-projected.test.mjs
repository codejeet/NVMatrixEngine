import test from 'node:test';
import assert from 'node:assert/strict';

// Independent finite-volume reference: rates are already open-area-weighted,
// oriented m^3/s, not point velocities. All conserved quantities share a flux.
function transport(q, oldCapacity, faces, dt) {
  const outgoing = oldCapacity.map(() => 0);
  for (const {l,r,rate} of faces) outgoing[rate >= 0 ? l : r] += Math.abs(rate);
  const scale = outgoing.map((v,i) => oldCapacity[i] > 0 ? Math.min(1,.95 / Math.max(dt*v/oldCapacity[i],1e-30)) : 0);
  const next = q.map(v => v.slice());
  for (const {l,r,rate} of faces) {
    const d = rate >= 0 ? l : r;
    const factor = oldCapacity[d] > 0 ? dt*rate*scale[d]/oldCapacity[d] : 0;
    for (let a=0; a<4; ++a) {
      const transfer = factor*q[d][a];
      next[l][a] -= transfer; next[r][a] += transfer;
    }
  }
  return {next,scale};
}
const close = (a,b,tol=1e-12) => assert.ok(Math.abs(a-b)<tol,`${a} != ${b}`);
const total = q => q.reduce((a,b) => a.map((v,i)=>v+b[i]),[0,0,0,0]);

test('swept cut capacities obey GCL and preserve uniform fraction/velocity', () => {
  const old=[.19,.32,.08,.27], dt=.013;
  const faces=[{l:0,r:1,rate:.31},{l:1,r:2,rate:-.17},{l:2,r:3,rate:.12},{l:3,r:0,rate:-.08}];
  const nextCapacity=old.slice();
  for (const f of faces) { nextCapacity[f.l]-=dt*f.rate; nextCapacity[f.r]+=dt*f.rate; }
  for (const fraction of [.03,.5,1]) {
    const velocity=[.7,-.3,.13], q=old.map(v=>[...velocity.map(u=>u*v*fraction),v*fraction]);
    const {next,scale}=transport(q,old,faces,dt);
    assert.ok(scale.every(v=>v===1));
    next.forEach((v,i)=>v.forEach((x,a)=>close(x,nextCapacity[i]*fraction*(a===3?1:velocity[a]))));
    total(next).forEach((v,a)=>close(v,total(q)[a]));
    // Regression traps: current instead of old capacity, or a second aperture
    // multiplication, conserve global mass but break constant-fraction transport.
    const wrongVolume=transport(q,nextCapacity,faces,dt).next;
    const wrongArea=transport(q,old,faces.map(f=>({...f,rate:f.rate*.4})),dt).next;
    for (const wrong of [wrongVolume,wrongArea]) assert.ok(wrong.some((v,i)=>Math.abs(v[3]-nextCapacity[i]*fraction)>1e-6));
  }
});

test('cached geometry does not replay an old swept-volume endpoint', () => {
  const current=[.2,.5], stale=[.1,.6], q=current.map(v=>[v*2,v*3,-v,v]);
  const faces=[{l:0,r:1,rate:.3},{l:1,r:0,rate:.3}];
  transport(q,current,faces,.01).next.forEach((v,i)=>v.forEach((x,a)=>close(x,q[i][a])));
  assert.ok(transport(q,stale,faces,.01).next.some((v,i)=>Math.abs(v[3]-q[i][3])>1e-6));
});

test('tiny cut donors stay positive and conserve momentum; zero capacity is not clamped away', () => {
  const capacity=[1e-12,.07,.23,0], q=capacity.map((v,i)=>[v*(i-2),v*.7,-v,v]);
  q[3]=[.012,-.003,0,.002]; // deliberately inconsistent inventory must remain observable
  const faces=[{l:0,r:1,rate:1},{l:0,r:2,rate:2},{l:1,r:2,rate:-100},{l:3,r:1,rate:4}];
  const {next,scale}=transport(q,capacity,faces,.01);
  assert.ok(scale[0]<1 && scale[2]<1 && scale[3]===0);
  assert.ok(next.every(v=>v[3]>=0&&v.every(Number.isFinite)));
  total(next).forEach((v,a)=>close(v,total(q)[a]));
  assert.deepEqual(next[3],q[3]);
});

test('donor limiting is conservative but is not a bounded VOF or adaptive timestep', () => {
  // GCL-compatible target capacities. The donor limiter prevents emptying but
  // breaks constant fraction, so a passing mass sum must never authorize handoff.
  const old=[1,1], faces=[{l:0,r:1,rate:.99}], dt=1;
  const {next,scale}=transport(old.map(v=>[0,0,0,v]),old,faces,dt);
  assert.ok(scale[0]<1); close(total(next)[3],2);
  assert.ok(next[0][3]>.01 && next[1][3]<1.99);
});

test('FP64 restriction retains cancellation lost by the extrapolated FP32 cache', () => {
  const fine=[1+2**-28,-1,2**-30,-(2**-31)];
  const exact=fine.reduce((a,b)=>a+b,0);
  const cache=fine.reduce((a,b)=>Math.fround(a+Math.fround(b)),0);
  close(exact,2**-28+2**-31,1e-20);
  assert.ok(Math.abs(exact-cache)>1e-9);
});
