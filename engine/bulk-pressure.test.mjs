import test from 'node:test';
import assert from 'node:assert/strict';
const full=(water,capacity)=>capacity>0&&water>=capacity*(1-1e-6);
function cell(type,water,start,current,previous,temporal){
  const volume=temporal?.5*(current+previous):current;
  return type===0&&volume>0&&full(water,start)?1:type;
}
function face(before,samples,open=true){
  if(before[2]>1e-8||!open)return before.slice();
  let v=0,m=0;
  for(const s of samples)if(s.filled&&s.volume>0){v+=s.volume*.5;m+=s.volume*.5*s.velocity;}
  return v>0?[m/v,m/v,v,0]:before.slice();
}
test('pressure support comes from admitted filled volume, not tiny fractional tails',()=>{
  assert.equal(cell(0,1,1,1,1,false),1);
  assert.equal(cell(0,.2,1,1,1,false),0);
  assert.equal(cell(0,1e-10,1,1,1,false),0);
  assert.equal(cell(0,5,0,0,0,true),0); // inconsistent closed inventory is not permission to invent space
  assert.equal(cell(1,.2,1,1,1,false),1); // partial surfaces still have particles
  assert.equal(cell(2,1,1,1,1,true),2); // never bypass the actual pressure solid classification
});
test('closing liquid can supply a temporal pressure unknown without changing endpoint capacity',()=>{
  assert.equal(cell(0,1,1,0,1,false),0);
  assert.equal(cell(0,1,1,0,1,true),1);
});
test('implicit evacuation retains partial closing mixtures but not open fractional tails',()=>{
  const fraction=(water,old,next,implicit,swept)=>{
    const start=swept?old:next;
    if(start<=0)return 0;
    if(full(water,start))return 1;
    return implicit&&swept&&next===0&&water>0?water/start:0;
  };
  assert.equal(fraction(.0020749,.032768,0,true,true),.0020749/.032768);
  assert.equal(fraction(.2,1,0,false,true),0);
  assert.equal(fraction(.2,1,1,true,true),0);
  assert.equal(fraction(.2,1,0,true,false),0);
  assert.equal(fraction(0,1,0,true,true),0);
  assert.equal(fraction(.2,0,0,true,true),0);
  assert.equal(fraction(1,1,0,true,true),1);
});
test('carrier mode evacuates threatened shrinking mixtures before their capacity reaches zero',()=>{
  const threatened=(water,previous,next)=>previous>0&&next<previous&&water>next;
  assert.equal(threatened(2.91328e-7,5e-7,1.82669e-7),true);
  assert.equal(threatened(.2,1,.3),false); // still enough room
  assert.equal(threatened(.2,.1,.3),false); // opening, not shrinking
  assert.equal(threatened(0,1,0),false); // no invented water
  assert.equal(threatened(.2,1,0),true);
});
test('Eulerian face coverage preserves the pre-force velocity and every valid P2G value',()=>{
  const samples=[{filled:true,volume:2,velocity:3},{filled:true,volume:1,velocity:-3}];
  assert.deepEqual(face([0,0,0,0],samples),[1,1,1.5,0]);
  const original=[7,-2,.01,1];assert.deepEqual(face(original,samples),original);
  assert.deepEqual(face([0,0,0,0],samples,false),[0,0,0,0]);
  assert.deepEqual(face([0,0,0,0],samples.map(s=>({...s,filled:false}))),[0,0,0,0]);
});
test('missing-face interpolation is a convex velocity average, not a second summed momentum field',()=>{
  let seed=14;const random=()=>((seed=Math.imul(seed,1664525)+1013904223>>>0)/2**32);
  for(let i=0;i<500;i++){
    const samples=[0,1].map(()=>({filled:true,volume:.0001+random(),velocity:40*(random()-.5)}));
    const f=face([0,0,0,0],samples),lo=Math.min(...samples.map(s=>s.velocity)),hi=Math.max(...samples.map(s=>s.velocity));
    assert.ok(f[0]>=lo-1e-12&&f[0]<=hi+1e-12);assert.equal(f[0],f[1]);
    const energy=samples.reduce((a,s)=>a+.25*s.volume*s.velocity*s.velocity,0);
    assert.ok(.5*f[2]*f[0]*f[0]<=energy+1e-10);
  }
});
test('a new bulk pressure row remains coupled to a particle row and the air boundary',()=>{
  // Two liquid cells: closed left wall, one shared face, atmospheric right edge.
  // The first cell has no particle, but is full in the transported bulk field.
  assert.equal(cell(0,1,1,1,1,false),1);
  const u=[0,2,-1],rhs=[-(u[1]-u[0]),-(u[2]-u[1])];
  // A = [1 -1; -1 2]. Solve independently, with dt/rho/h = 1.
  const p=[2*rhs[0]+rhs[1],rhs[0]+rhs[1]];
  const projected=[0,u[1]-(p[1]-p[0]),u[2]+p[1]];
  assert.ok(Math.abs(projected[1]-projected[0])<1e-12);
  assert.ok(Math.abs(projected[2]-projected[1])<1e-12);
});
test('capillary occupancy uses a union, not double-counted water',()=>{
  const occupancy=(particle,filled)=>Math.max(Math.min(1,particle),filled);
  assert.equal(occupancy(.7,1),1);assert.equal(occupancy(1,1),1);
  assert.equal(occupancy(.2,0),.2);assert.equal(occupancy(0,.4),.4);
});
test('a particle-free bin can be compressed by neighboring interpolation kernels',()=>{
  const quadratic=x=>Math.abs(x)<.5?.75-x*x:Math.abs(x)<1.5?.5*(1.5-Math.abs(x))**2:0;
  const points=[0,1,2].flatMap(axis=>[-.6,.6].map(v=>{const p=[0,0,0];p[axis]=v;return p;}));
  assert.ok(points.every(p=>p.some(v=>Math.abs(v)>.5)),'No center lies in the audited cell');
  const measuredMass=points.reduce((sum,p)=>sum+p.reduce((w,x)=>w*quadratic(x),1),0);
  assert.ok(measuredMass>1.02,'Kernel overlap triggers the existing density error threshold');
  const occupied=(binCount,filledSupport)=>binCount>0||filledSupport>0;
  assert.equal(occupied(0,0),false);assert.equal(occupied(0,1),true);
  // Coverage changes connectivity only; it is NOT added to the measured RHS.
  const rhs=Math.min(Math.max(measuredMass-1,0),.5);
  assert.ok(rhs>0&&rhs<.5);
});
