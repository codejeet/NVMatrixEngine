import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
const value=(volume,velocity=[1,-.3,.7])=>[...velocity.map(v=>v*volume),volume];
const sum=q=>q.reduce((s,v)=>s.map((x,a)=>x+v[a]),[0,0,0,0]);
const energy=q=>q.reduce((s,v)=>s+(v[3]>.0?.5*v.slice(0,3).reduce((x,a)=>x+a*a,0)/v[3]:0),0);
function allocate(resident,pending,capacity,edges,iterations=64,tolerance=0,advance=true){
  resident=resident.map(v=>v.slice());pending=pending.map(v=>v.slice());
  const weights=capacity.map(()=>0);
  for(const {l,r,area} of edges){if(capacity[r]>0)weights[l]+=area;if(capacity[l]>0)weights[r]+=area;}
  function accept(){for(let i=0;i<resident.length;i++){
    const amount=Math.min(pending[i][3],Math.max(0,capacity[i]-resident[i][3]));
    const factor=pending[i][3]>0?amount/pending[i][3]:0;
    for(let a=0;a<4;a++){const transfer=a===3?amount:pending[i][a]*factor;resident[i][a]+=transfer;pending[i][a]-=transfer;}
  }}
  if(advance)accept();
  for(let iteration=0;advance&&iteration<iterations;iteration++){
    if(!pending.some((v,i)=>v[3]>tolerance&&weights[i]>0))break;
    const next=pending.map(v=>v.slice());
    for(const {l,r,area} of edges){
      for(let a=0;a<4;a++){
        const fromL=weights[l]>0&&capacity[r]>0?pending[l][a]*.5*area/weights[l]:0;
        const fromR=weights[r]>0&&capacity[l]>0?pending[r][a]*.5*area/weights[r]:0;
        const transfer=fromL-fromR;next[l][a]-=transfer;next[r][a]+=transfer;
      }
    }
    pending=next;accept();
  }
  return {resident,pending};
}
function invariants(beforeR,beforeP,capacity,result){
  const {resident,pending}=result,a=sum([...beforeR,...beforeP]),b=sum([...resident,...pending]);
  a.forEach((v,i)=>assert.ok(Math.abs(v-b[i])<1e-10));
  resident.forEach((v,i)=>assert.ok(v[3]>=0&&v[3]<=Math.max(capacity[i],beforeR[i][3])+1e-12));
  pending.forEach(v=>assert.ok(v[3]>=0&&v.every(Number.isFinite)));
  assert.ok(energy([...resident,...pending])<=energy([...beforeR,...beforeP])+1e-10);
}
test('source allocation fits oversubscribed initial cells without discarding any quantity',()=>{
  const c=[1,.25,.75,1],r=c.map(()=>value(0)),p=[value(2),value(0),value(0),value(0)];
  const edges=[{l:0,r:1,area:.5},{l:1,r:2,area:.25},{l:2,r:3,area:.7}];
  const result=allocate(r,p,c,edges,256);invariants(r,p,c,result);
  assert.ok(sum(result.pending)[3]<1e-6);assert.ok(result.resident[2][3]>.5);
});
test('multiple momentum-carrying sources share receiver capacity without energy injection',()=>{
  const c=[.2,.25,.3],r=c.map(()=>value(0)),p=[value(.4,[4,-1,3]),value(0),value(.35,[-2,3,1])];
  const result=allocate(r,p,c,[{l:0,r:1,area:.5},{l:1,r:2,area:.9}],256);
  invariants(r,p,c,result);assert.ok(sum(result.pending)[3]<1e-6);
});
test('insufficient or disconnected capacity leaves explicit pending inventory',()=>{
  const c=[.2,.3,10,0],r=c.map(()=>value(0)),p=[value(1),value(0),value(0),value(.2)];
  const result=allocate(r,p,c,[{l:0,r:1,area:.5},{l:1,r:2,area:0},{l:2,r:3,area:0}],256);
  invariants(r,p,c,result);
  assert.ok(Math.abs(sum(result.pending)[3]-.7)<1e-10);
  assert.equal(result.resident[2][3],0);assert.deepEqual(result.pending[3],p[3]);
});
test('admission never repairs or increases pre-existing advection overfill',()=>{
  const c=[1,1,1],r=[value(1.4),value(.2),value(0)],p=[value(.3),value(0),value(0)];
  const result=allocate(r,p,c,[{l:0,r:1,area:.4},{l:1,r:2,area:.3}]);
  invariants(r,p,c,result);assert.deepEqual(result.resident[0],r[0]);
  assert.ok(result.resident[1][3]>.49);
});
test('empty and closed tiny cells neither fabricate capacity nor lose sub-cell quantities',()=>{
  const c=[0,1e-12,.5],r=c.map(()=>value(0)),p=[value(0),value(1e-9),value(0)];
  const result=allocate(r,p,c,[{l:0,r:1,area:0},{l:1,r:2,area:1e-8}]);
  invariants(r,p,c,result);assert.equal(result.resident[0][3],0);
  assert.ok(result.resident[1][3]<=1e-12&&result.resident[2][3]>9e-10);
});
test('GPU allocator separates pending admission from resident simulation and optical authority',()=>{
  const source=readFileSync(new URL('./shaders/fluid/volume-allocation.hlsl',import.meta.url),'utf8');
  assert.doesNotMatch(source,/Particle|LiquidPhi|SurfaceField/);
  assert.match(source,/q-=admitted/);assert.match(source,/r\+=admitted/);
  const system=readFileSync(new URL('./src/fluid/fluid_system.cpp',import.meta.url),'utf8');
  assert.ok(system.indexOf('bulk->allocateSources(cmd, advanceAdmission)')<system.indexOf('collidersDirty = false;\n    for'));
});
test('sub-tolerance pending mass remains accounted for and is admitted when capacity opens',()=>{
  const r=[value(1),value(0)],p=[value(1e-12),value(0)],c=[1,1],edges=[{l:0,r:1,area:.5}];
  const frozen=allocate(r,p,c,edges,64,1e-10);invariants(r,p,c,frozen);
  frozen.pending.forEach((v,i)=>v.forEach((x,a)=>assert.ok(x===p[i][a])));
  const opened=allocate(frozen.resident,frozen.pending,[2,1],edges,64,1e-10);
  assert.equal(sum(opened.pending)[3],0);
  assert.ok(Math.abs(sum(opened.resident)[3]-1-1e-12)<1e-15);
});
test('paused inspection is a read-only transaction even when source capacity is available',()=>{
  const r=[value(.4)],p=[value(.1)],c=[1];
  const paused=allocate(r,p,c,[],64,1e-10,false);
  assert.deepEqual(paused.resident,r);assert.deepEqual(paused.pending,p);
});
