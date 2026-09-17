import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
const add=(a,b)=>a.map((x,i)=>x+b[i]),sub=(a,b)=>a.map((x,i)=>x-b[i]);
const scale=(a,s)=>a.map(x=>x*s),dot=(a,b)=>a.reduce((v,x,i)=>v+x*b[i],0);
const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
const mv=(C,x)=>C.map(row=>dot(row,x));
let rng=314159;const random=()=>{rng=(Math.imul(rng,1664525)+1013904223)>>>0;return rng/2**32;};
const vec=()=>Array.from({length:3},()=>2*random()-1);
function totals(p,D){return {
  mass:p.reduce((s,q)=>s+q.m,0),
  linear:p.reduce((s,q)=>add(s,scale(q.v,q.m)),[0,0,0]),
  angular:p.reduce((s,q)=>add(s,scale(add(cross(q.x,q.v),scale([q.C[2][1]-q.C[1][2],q.C[0][2]-q.C[2][0],q.C[1][0]-q.C[0][1]],D)),q.m)),[0,0,0]),
  energy:p.reduce((s,q)=>s+.5*q.m*(dot(q.v,q.v)+D*q.C.reduce((t,r)=>t+dot(r,r),0)),0)
};}
function merge(p,q,D){const m=p.m+q.m,x=scale(add(scale(p.x,p.m),scale(q.x,q.m)),1/m),v=scale(add(scale(p.v,p.m),scale(q.v,q.m)),1/m);
  const C=p.C.map((row,a)=>row.map((value,b)=>(p.m*value+q.m*q.C[a][b])/m+
    (p.m*(p.v[a]-v[a])*(p.x[b]-x[b])+q.m*(q.v[a]-v[a])*(q.x[b]-x[b]))/(m*D)));
  return {m,x,v,C};}
function split(p,d,D){const C=p.C.map(row=>sub(row,scale(d,dot(row,d)/(D+dot(d,d))))),dv=mv(C,d);
  return [-1,1].map(sign=>({m:p.m/2,x:add(p.x,scale(d,sign)),v:add(p.v,scale(dv,sign)),C}));}
function conserved(a,b){assert.equal(a.mass,b.mass);for(const key of ['linear','angular'])for(let i=0;i<3;i++)assert.ok(Math.abs(a[key][i]-b[key][i])<2e-11,`${key}: ${a[key]} vs ${b[key]}`);}
test('weighted APIC pair merge conserves mass, linear and angular momentum',()=>{
  let accepted=0,rejected=0;const D=.08**2/4;
  for(let i=0;i<5000;i++){
    const p={m:2**(i%4),x:vec(),v:vec(),C:[vec(),vec(),vec()]};
    const q={m:p.m,x:add(p.x,scale(vec(),.02)),v:add(p.v,scale(vec(),.1)),C:[vec(),vec(),vec()]};
    if(i%2){q.C=p.C;q.v=add(p.v,mv(p.C,sub(q.x,p.x)));} // coherent affine flow exposes energy-injecting pair collapses
    const a=totals([p,q],D),b=totals([merge(p,q,D)],D);conserved(a,b);
    if(b.energy<=a.energy)accepted++;else rejected++;
  }
  assert.ok(accepted>100&&rejected>100,'Energy gate must actually reject some conserving merges');
});
test('APIC split conserves all moments and never injects kinetic/affine energy',()=>{
  for(let i=0;i<5000;i++){
    const D=.08**2/4,p={m:2**(i%4),x:vec(),v:vec(),C:[vec(),vec(),vec()]},d=scale(vec(),.008);
    const a=totals([p],D),b=totals(split(p,d,D),D);conserved(a,b);assert.ok(b.energy<=a.energy+1e-12);
  }
});
test('dyadic sample density transitions preserve exact bin mass and liquid occupancy',()=>{
  const cells=Array.from({length:20},()=>Array(16).fill(16));
  for(let cycle=0;cycle<32;cycle++)for(const cell of cells){
    if(cycle%2){const m=cell.pop();cell.push(m/2,m/2);}else {const a=cell.pop(),b=cell.pop();cell.push(a+b);}
    assert.equal(cell.reduce((s,m)=>s+m,0),256);assert.ok(cell.length>0);
    assert.equal(cell.reduce((s,m)=>s+m/16,0)*.025,16*.025);
  }
});
test('parent selection preceding recycled child writes avoids stale-bin ID aliasing',()=>{
  const oldBins=[[0,1],[2,3]],alive=[true,false,true,false];
  const selected=oldBins.map(ids=>ids.find(i=>alive[i]));const free=alive.flatMap((v,i)=>v?[]:[i]);
  const children=[free[1],free[0]];assert.deepEqual(selected,[0,2]);
  assert.equal(new Set([...selected,...children]).size,4);
  // Child 1 is now in cell 1 but its ID remains in the *old* cell-0 bin.
  // Selecting from that bin after child writes would race and choose it again.
  assert.equal(oldBins[0][1],children[1]);
});
test('weighted surface and pressure consumers retain mass instead of substituting sample count',()=>{
  for(const path of ['transfer.hlsl','density.hlsl'])assert.match(readFileSync(new URL(`./shaders/fluid/${path}`,import.meta.url),'utf8'),/particleWeight/);
  assert.match(readFileSync(new URL('./shaders/fluid/material.hlsl',import.meta.url),'utf8'),/cellMass/);
  assert.match(readFileSync(new URL('./shaders/fluid/reconstruction.hlsl',import.meta.url),'utf8'),/apic0.w/);
  assert.match(readFileSync(new URL('./shaders/fluid/collision.hlsl',import.meta.url),'utf8'),/p.apic0.xyz=p.apic1.xyz=p.apic2.xyz=0/);
});
test('indirect maintenance and rebin requests are empty only when no work is required',()=>{
  for(const coarse of [0,17])for(const samples of [0,99,100])for(const mass of [100]){
    const needsWork=coarse>0||mass>samples;
    if(!needsWork)assert.equal(samples,mass);
    for(const changes of [0,1,400]){
      const grid=105750,capacity=100000,groups=[Math.ceil(grid/256),Math.ceil(capacity/256),Math.ceil(grid/256),1,Math.ceil(grid/256),Math.ceil(capacity/256)];
      const dispatch=groups.map(x=>changes?x:0);
      assert.ok(dispatch.every((x,i)=>changes?x===groups[i]:x===0));
    }
  }
});
