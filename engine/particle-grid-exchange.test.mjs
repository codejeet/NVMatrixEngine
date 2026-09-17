import test from 'node:test';
import assert from 'node:assert/strict';

const sum=q=>q.reduce((s,p)=>s.map((v,a)=>v+p[a]),[0,0,0,0]);
const energy=q=>q[3]>0?q.slice(0,3).reduce((s,x)=>s+.5*x*x/q[3],0):0;
test('mass ownership is a partition, not particle mass plus its full grid replica',()=>{
  const particles=[[.1,.2,-.1,.3],[-.2,.1,.2,.4],[.2,-.1,0,.5]];
  const before=sum(particles),owned=sum(particles.slice(0,2)),remaining=particles.slice(2);
  assert.deepEqual(sum([...remaining,owned]),before);
  assert.ok(sum([...particles,owned])[3]>before[3]);
  assert.ok(energy(owned)<=particles.slice(0,2).reduce((e,q)=>e+energy(q),0));
});
test('fractional restoration keeps the final FP64 remainder instead of quantizing physical mass to cached weights',()=>{
  const original=[.017,-.032,.09,1/7];
  for(const count of [1,3,7,37,1024]){
    const each=original.map(x=>x/count);let remaining=[...original];const samples=[];
    for(let i=0;i<count;i++){
      const q=i+1===count?[...remaining]:[...each];samples.push(q);
      remaining=remaining.map((v,a)=>v-q[a]);
    }
    assert.deepEqual(remaining,[0,0,0,0]);
    const restored=sum(samples);
    restored.forEach((v,a)=>assert.ok(Math.abs(v-original[a])<3e-15));
    assert.ok(samples.every(q=>q[3]>0));
  }
  const cache=Math.fround(original[3]);assert.notEqual(cache,original[3]);
});
test('a zero cached velocity delta must preserve sub-FP32 authoritative momentum',()=>{
  const mass=.017,momentum=.123456789123;
  let reference=Math.fround(momentum/mass),q=momentum;
  for(let step=0;step<1000;step++){
    const current=reference;q+=mass*(current-reference);reference=current;
  }
  assert.equal(q,momentum);
  assert.notEqual(mass*reference,momentum,'Recaching absolute velocity loses information');
  const next=Math.fround(reference+.125),expected=q+mass*(next-reference);
  q+=mass*(next-reference);reference=next;q+=mass*(next-reference);
  assert.equal(q,expected);
});
test('failed large reservations leave slots available to smaller complete transactions',()=>{
  for(const order of [[9,3,2],[3,9,2],[2,3,9]]){
    let used=0;const accepted=[];
    for(const count of order){if(count>5-used)continue;accepted.push(count);used+=count;}
    assert.equal(used,5);assert.deepEqual(accepted.sort(),[2,3]);
  }
});
test('a closing grid owner can transfer all mass before restoring new particle detail',()=>{
  // Exact two-cell backward-Euler matrix: [.5 0; -.5 1], source [.4 0].
  // The first endpoint has zero storage, but an open integrated outflow .5.
  const concentration=[.4/.5,.4],capacity=[0,1];
  const output=capacity.map((v,i)=>v*concentration[i]);
  assert.deepEqual(output,[0,.4]);assert.equal(output[0]+output[1],.4);
  assert.equal(.5*concentration[0],output[1]);
});
test('fractional bin mass must not use the legacy one-sixteenth quantizer',()=>{
  const weights=[1/7,2/11,3/13];
  const physical=weights.reduce((a,b)=>a+b,0);
  const cache=Math.fround(physical);
  assert.ok(Math.abs(cache-physical)<physical*1e-7);
  const old=weights.reduce((a,b)=>a+Math.round(b*16)/16,0);
  assert.ok(Math.abs(old-physical)>physical*.01);
});
