import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';

function grid(n,width){
  const size=n.reduce((s,v)=>s*v),xyz=Array.from({length:size},(_,i)=>[i%n[0],Math.floor(i/n[0])%n[1],Math.floor(i/(n[0]*n[1]))]);
  const id=p=>(p[2]*n[1]+p[1])*n[0]+p[0];
  const extent=p=>p.map((v,a)=>Math.min(1,width[a]-v));
  const volume=xyz.map(p=>extent(p).reduce((s,v)=>s*v));
  const faces=[];
  // Unique oriented interior faces; periodic seam uses exactly one shared edge.
  for(let i=0;i<size;i++)for(let a=0;a<3;a++){
    const p=xyz[i],r=p.slice();r[a]=(r[a]+1)%n[a];
    const area=extent(p).filter((_,k)=>k!==a).reduce((s,v)=>s*v);
    faces.push({l:i,r:id(r),axis:a,area,seam:p[a]===n[a]-1});
  }
  return {n,size,xyz,id,volume,faces};
}
function advance(g,state,vel,dt,periodic=true){
  const rates=g.faces.map((f,i)=>!periodic&&f.seam?0:vel(f,i)*f.area);
  const out=new Float64Array(g.size);
  g.faces.forEach((f,i)=>{out[rates[i]>=0?f.l:f.r]+=dt*Math.abs(rates[i])/g.volume[rates[i]>=0?f.l:f.r];});
  const scale=out.map(v=>Math.min(1,.95/Math.max(v,1e-30)));
  const next=state.map(v=>v.slice());
  g.faces.forEach((f,i)=>{
    const d=rates[i]>=0?f.l:f.r,factor=dt*rates[i]*scale[d]/g.volume[d];
    for(let c=0;c<4;c++){const transfer=factor*state[d][c];next[f.l][c]-=transfer;next[f.r][c]+=transfer;}
  });
  return {next,scale};
}
const sum=q=>q.reduce((s,v)=>s.map((x,k)=>x+v[k]),[0,0,0,0]);
test('bulk shared flux conserves all four quantities and positivity at high multi-axis CFL',()=>{
  for(const periodic of [true,false]){
    const g=grid([7,5,9],[6.2,4.7,8.15]);
    let q=g.xyz.map(([x,y,z],i)=>{const m=(x+y+z)%4?g.volume[i]*(.1+.6*(Math.sin(i)+1)):0;return [m*Math.sin(x),m*Math.cos(y),-m*.3,m];});
    const before=sum(q);let limited=false;
    for(let s=0;s<80;s++){
      const r=advance(g,q,(f,i)=>Math.sin(i*.7+s*.01)*25,.4,periodic);q=r.next;
      limited ||= r.scale.some(v=>v<1);assert.ok(q.every(v=>v[3]>=0&&v.every(Number.isFinite)));
    }
    assert.ok(limited);
    sum(q).forEach((v,k)=>assert.ok(Math.abs(v-before[k])<1e-9));
  }
});
test('periodic constant velocity preserves uniform fractions and momentum on partial cells',()=>{
  const g=grid([7,3,5],[6.2,2.7,4.15]),u=[.7,-.2,.3];
  let q=g.volume.map(v=>[...u.map(x=>x*v*.4),v*.4]);const initial=q.map(v=>v.slice());
  for(let i=0;i<120;i++)q=advance(g,q,f=>u[f.axis],.02).next;
  q.forEach((v,i)=>v.forEach((x,k)=>assert.ok(Math.abs(x-initial[i][k])<1e-12)));
});
test('coarse area restriction commutes with flux divergence on odd fine dimensions',()=>{
  const n=[7,5,9],N=n.map(v=>Math.ceil(v/2)),id=p=>(p[2]*n[1]+p[1])*n[0]+p[0];
  const cId=p=>(p[2]*N[1]+p[1])*N[0]+p[0],rhs=new Float64Array(N.reduce((a,b)=>a*b)),restricted=new Float64Array(rhs.length);
  for(let z=0;z<n[2];z++)for(let y=0;y<n[1];y++)for(let x=0;x<n[0];x++)for(let a=0;a<3;a++){
    const l=[x,y,z],r=l.slice();r[a]++;if(r[a]>=n[a])continue;
    const rate=Math.sin(id(l)*.71+a)*(.2+(a+1)*.1); // arbitrary already-area-weighted fine flux
    const L=cId(l.map(v=>Math.floor(v/2))),R=cId(r.map(v=>Math.floor(v/2)));
    rhs[L]+=rate;rhs[R]-=rate;
    if(L!==R){restricted[L]+=rate;restricted[R]-=rate;}
  }
  rhs.forEach((v,i)=>assert.ok(Math.abs(v-restricted[i])<1e-12));
});
test('birth ranges inject once, reset replaces inventory, and excess volume remains visible',()=>{
  const particles=Array.from({length:50},(_,i)=>({cell:i%7,v:[.7,i*.01,-.3]}));
  let q=Array.from({length:7},()=>[0,0,0,0]),ledger=q.map(v=>v.slice());const pv=.03;
  function source(first,count,reset){if(reset){q=q.map(()=>[0,0,0,0]);ledger=q.map(v=>v.slice());}
    for(let i=first;i<first+count;i++)for(let c=0;c<4;c++){const d=pv*(c===3?1:particles[i].v[c]);q[particles[i].cell][c]+=d;ledger[particles[i].cell][c]+=d;}}
  source(0,20,true);source(20,5,false);source(0,0,false);
  assert.ok(Math.abs(sum(q)[3]-25*pv)<1e-14);assert.deepEqual(q,ledger);
  source(0,20,true);assert.ok(Math.abs(sum(q)[3]-20*pv)<1e-14);
  assert.ok(q.some(v=>v[3]/.02>1),'do not clamp fractional overfill and silently destroy mass');
});
test('GPU bulk path never writes the authoritative particle or optical representation',()=>{
  const shader=readFileSync(new URL('./shaders/fluid/bulk.hlsl',import.meta.url),'utf8');
  assert.doesNotMatch(shader,/Particles\s*\[[^\]]+\]\s*(?:\.[a-z]+)?\s*(?:=|\+=|-=)/);
  assert.doesNotMatch(shader,/StateOut\[id\]\s*=\s*(?:saturate|max|clamp)/);
  const system=readFileSync(new URL('./src/fluid/fluid_system.cpp',import.meta.url),'utf8');
  assert.match(system,/if \(bulkSourcesPending\)/);
  assert.match(system,/bulkSourcesPending = false/);
});
