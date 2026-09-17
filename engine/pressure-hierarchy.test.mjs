import test from 'node:test';
import assert from 'node:assert/strict';

function operator(n,kind){
  const size=n.reduce((a,b)=>a*b),coords=Array.from({length:size},(_,i)=>[i%n[0],Math.floor(i/n[0])%n[1],Math.floor(i/(n[0]*n[1]))]);
  const types=coords.map(p=>kind(...p)),rows=Array.from({length:size},()=>({d:0,neighbors:[]}));
  const index=p=>(p[2]*n[1]+p[1])*n[0]+p[0];
  for(let i=0;i<size;i++)if(types[i]===1)for(let a=0;a<3;a++)for(const side of [-1,1]){
    const q=coords[i].slice();q[a]+=side;
    if(q.some((v,k)=>v<0||v>=n[k]))continue;
    const j=index(q);if(types[j]===2)continue;
    rows[i].d++;if(types[j]===1)rows[i].neighbors.push([j,1]);
  }
  return {n,size,coords,types,rows};
}
const apply=(a,x)=>Float64Array.from(a.rows,(row,i)=>row.d*x[i]-row.neighbors.reduce((s,[j,w])=>s+w*x[j],0));
const rms=x=>Math.sqrt(x.reduce((s,v)=>s+v*v,0)/Math.max(x.length,1));
function aggregate(a){
  const n=a.n.map(v=>Math.ceil(v/2)),size=n.reduce((s,v)=>s*v);
  const parent=a.coords.map(p=>(Math.floor(p[2]/2)*n[1]+Math.floor(p[1]/2))*n[0]+Math.floor(p[0]/2));
  const matrix=Array.from({length:size},()=>new Map()),children=Array.from({length:size},()=>[]);
  // Independent sparse matrix product P^T A P, not the shader's 8-child stencil construction.
  for(let i=0;i<a.size;i++)if(a.rows[i].d){
    const p=parent[i];children[p].push(i);matrix[p].set(p,(matrix[p].get(p)||0)+a.rows[i].d);
    for(const [j,w]of a.rows[i].neighbors){const q=parent[j];matrix[p].set(q,(matrix[p].get(q)||0)-w);}
  }
  return {n,size,parent,children,rows:matrix.map((m,i)=>({d:m.get(i)||0,neighbors:[...m].filter(([j,w])=>j!==i&&w).map(([j,w])=>[j,-w])}))};
}
function relax(a,b,x,sweeps,omega=1,active=false){
  const ids=Array.from({length:a.size},(_,i)=>i).filter(i=>!active||a.rows[i].d).reverse();
  for(let t=0;t<sweeps;t++){
    const next=new Float64Array(a.size);
    for(const i of ids){const r=a.rows[i];if(!r.d)continue;
      const j=(b[i]+r.neighbors.reduce((s,[k,w])=>s+w*x[k],0))/r.d;
      next[i]=omega===1?j:x[i]+omega*(j-x[i]);}
    x=next;
  }
  return x;
}
function vcycle(a,b,cycles=3){
  const c=aggregate(a);let x=new Float64Array(a.size);
  for(let cycle=0;cycle<cycles;cycle++){
    x=relax(a,b,x,4,2/3,true);const ax=apply(a,x);
    const restricted=Float64Array.from(c.children,ids=>ids.reduce((s,i)=>s+b[i]-ax[i],0));
    const e=relax(c,restricted,new Float64Array(c.size),24,2/3,true);
    for(let i=0;i<a.size;i++)if(a.rows[i].d)x[i]+=e[c.parent[i]];
    x=relax(a,b,x,4,2/3,true);
  }
  return relax(a,b,x,48,2/3,true);
}
test('coarse aggregation preserves symmetry, energy and partial odd-grid boundary rows',()=>{
  for(const kind of [()=>1,(x,y,z)=>y>2?0:(x===3&&z<5?2:1),(x,y,z)=>(x+y+z)%7===0?2:((x+z)%5===0?0:1)]){
    const a=operator([7,5,9],kind),c=aggregate(a);
    for(let i=0;i<c.size;i++)for(const [j,w]of c.rows[i].neighbors){
      assert.equal(c.rows[j].neighbors.find(([k])=>k===i)?.[1],w);
      assert.ok(c.rows[i].d>=c.rows[i].neighbors.reduce((s,[,v])=>s+v,0));
    }
    const x=Float64Array.from({length:c.size},(_,i)=>Math.sin(i*.7));
    const px=Float64Array.from({length:a.size},(_,i)=>a.rows[i].d?x[c.parent[i]]:0);
    const fineEnergy=apply(a,px).reduce((s,v,i)=>s+v*px[i],0),coarseEnergy=apply(c,x).reduce((s,v,i)=>s+v*x[i],0);
    assert.ok(Math.abs(fineEnergy-coarseEnergy)<1e-9);
    assert.equal(c.children.flat().length,a.rows.filter(r=>r.d).length);
  }
});
test('active Jacobi exactly matches uniform sweeps with inhomogeneous air-pressure RHS',()=>{
  const a=operator([9,7,11],(x,y,z)=>y>3?0:((x===3&&z>4)?2:1));
  const b=Float64Array.from(a.rows,(r,i)=>r.d?Math.sin(i*.15)*700:0);
  const full=relax(a,b,new Float64Array(a.size),120),active=relax(a,b,new Float64Array(a.size),120,1,true);
  assert.deepEqual(full,active);
});
test('two-sweep tiles preserve halo dependencies, solids and partial edge tiles',()=>{
  for(const omega of [1,2/3])for(const n of [[1,3,7],[9,7,11],[17,9,5]]){
    const a=operator(n,(x,y,z)=>y===n[1]-1?0:((x===7&&z>1)?2:1));
    const b=Float64Array.from(a.rows,(r,i)=>r.d?Math.sin(i*.13)*17:0);
    const initial=Float64Array.from(a.rows,(r,i)=>r.d?Math.cos(i*.17):0);
    const expected=relax(a,b,initial,2,omega),blocked=new Float64Array(a.size);
    const first=i=>{
      const row=a.rows[i];if(!row.d)return 0;
      const j=(b[i]+row.neighbors.reduce((s,[k,w])=>s+w*initial[k],0))/row.d;
      return omega===1?j:initial[i]+omega*(j-initial[i]);
    };
    // Every tile computes its own halo from the original iterate. Reversing
    // tile order must not turn this into Gauss-Seidel or stale-halo Jacobi.
    for(let z=Math.floor((n[2]-1)/4)*4;z>=0;z-=4)for(let y=Math.floor((n[1]-1)/4)*4;y>=0;y-=4)for(let x=Math.floor((n[0]-1)/8)*8;x>=0;x-=8){
      const halo=new Map();
      for(let dz=-1;dz<=4;dz++)for(let dy=-1;dy<=4;dy++)for(let dx=-1;dx<=8;dx++){
        const p=[x+dx,y+dy,z+dz];if(p.some((v,k)=>v<0||v>=n[k]))continue;
        const id=(p[2]*n[1]+p[1])*n[0]+p[0];halo.set(id,first(id));
      }
      for(let dz=0;dz<4;dz++)for(let dy=0;dy<4;dy++)for(let dx=0;dx<8;dx++){
        const p=[x+dx,y+dy,z+dz];if(p.some((v,k)=>v>=n[k]))continue;
        const id=(p[2]*n[1]+p[1])*n[0]+p[0],r=a.rows[id];if(!r.d)continue;
        const j=(b[id]+r.neighbors.reduce((s,[k,w])=>s+w*halo.get(k),0))/r.d;
        blocked[id]=omega===1?j:halo.get(id)+omega*(j-halo.get(id));
      }
    }
    assert.deepEqual(blocked,expected);
  }
});
test('two-level pressure corrects smooth error and damps alternating-cell error',()=>{
  for(const n of [[16,12,18],[15,3,17]]){
    const a=operator(n,(x,y,z)=>y===n[1]-1?0:1);
    const exact=Float64Array.from(a.coords,([x,y,z],i)=>a.rows[i].d?Math.cos(x*.18)*Math.sin(z*.11)+.2*((x+y+z)%2?1:-1):0);
    const b=apply(a,exact),mg=vcycle(a,b),baseline=relax(a,b,new Float64Array(a.size),120);
    const error=x=>rms(apply(a,x).map((v,i)=>v-b[i]));
    assert.ok(error(mg)<error(baseline),`${n}: multigrid residual ${error(mg)} vs Jacobi ${error(baseline)}`);
  }
});
test('empty and isolated Neumann aggregates remain finite without a fake pressure anchor',()=>{
  const empty=operator([3,5,7],()=>0);
  assert.deepEqual(vcycle(empty,new Float64Array(empty.size)),new Float64Array(empty.size));
  const a=operator([2,2,2],()=>1),c=aggregate(a);
  assert.equal(c.rows[0].d,0);
  const exact=Float64Array.from({length:a.size},(_,i)=>Math.sin(i)),b=apply(a,exact),x=vcycle(a,b);
  assert.ok(x.every(Number.isFinite));
  assert.ok(rms(apply(a,x).map((v,i)=>v-b[i]))<rms(b)*.001);
});
