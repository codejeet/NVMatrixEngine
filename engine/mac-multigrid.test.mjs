import test from 'node:test';
import assert from 'node:assert/strict';
import {macMesh,dot,multiply} from './adaptive-mac-reference.mjs';
import {hierarchy,vcycle,pcg} from './mac-multigrid-reference.mjs';
const mesh=macMesh([20,16,20],(x,y,z)=>(x+2*y+z)%4!==0&&y>0);
// Positive pressure boundary removes the constant null mode for these tests.
mesh.nodes.forEach((n,i)=>{if(n.base[1]===0)mesh.rows[i].set(i,mesh.rows[i].get(i)+n.width);});
const levels=hierarchy(mesh),precondition=vcycle(levels);
test('mixed-grid geometric aggregation is Galerkin and preserves symmetry/energy',()=>{
  assert.ok(levels.length>=3);
  for(let l=0;l<levels.length-1;l++){
    const f=levels[l],c=levels[l+1],x=c.nodes.map((_,i)=>Math.sin(i*.37));
    const fine=f.parent.map(p=>x[p]);
    assert.ok(Math.abs(dot(x,multiply(c.rows,x))-dot(fine,multiply(f.rows,fine)))<1e-8);
    for(let i=0;i<c.rows.length;i++)for(const [j,a] of c.rows[i])assert.ok(Math.abs(a-(c.rows[j].get(i)??0))<1e-10);
  }
});
test('symmetric multigrid V-cycle is a positive-definite linear preconditioner',()=>{
  const a=mesh.nodes.map((_,i)=>Math.sin(i*.13)),b=mesh.nodes.map((_,i)=>Math.cos(i*.17));
  const pa=precondition(a),pb=precondition(b),pab=precondition(a.map((x,i)=>x+b[i]));
  assert.ok(Math.abs(dot(a,pb)-dot(b,pa))<1e-7);
  assert.ok(dot(a,pa)>0);
  assert.ok(pab.every((x,i)=>Math.abs(x-pa[i]-pb[i])<1e-9));
});
test('multigrid PCG converges to the actual mixed-grid pressure solution',()=>{
  const exact=mesh.nodes.map(n=>Math.sin(n.center[0]*.1)*Math.cos(n.center[2]*.2)+.02*n.center[1]);
  const rhs=multiply(mesh.rows,exact),result=pcg(mesh.rows,rhs,precondition,64,1e-8);
  assert.ok(result.relativeResidual<1e-8);
  assert.ok(result.iterations<32,`Needed ${result.iterations} iterations`);
  assert.ok(Math.max(...result.p.map((p,i)=>Math.abs(p-exact[i])))<1e-6);
});
test('irregular wet regions converge without replacing the fine operator',()=>{
  const selected=mesh.nodes.filter(n=>!(n.center[0]>6&&n.center[0]<12&&n.center[2]>7&&n.center[1]>4));
  const ids=new Map(selected.map((n,i)=>[n.id,i]));
  const irregular={nodes:selected,rows:selected.map(n=>new Map([...mesh.rows[n.id]].filter(([j])=>ids.has(j)).map(([j,a])=>[ids.get(j),a])))};
  const levels=hierarchy(irregular),exact=selected.map(n=>Math.cos(n.center[0]*.3)+Math.sin(n.center[2]*.2));
  const result=pcg(irregular.rows,multiply(irregular.rows,exact),vcycle(levels),64,1e-8);
  assert.ok(result.relativeResidual<1e-8&&result.iterations<32);
  assert.ok(Math.max(...result.p.map((v,i)=>Math.abs(v-exact[i])))<1e-6);
});
test('closed Neumann domain keeps its null mode out of the physical operator',()=>{
  const closed=macMesh([16,12,16],(x,y,z)=>(x+y+z)%3!==0);
  const exact=closed.nodes.map(n=>Math.sin(n.center[0]*.2)*Math.cos(n.center[1]*.3));
  const rhs=multiply(closed.rows,exact),result=pcg(closed.rows,rhs,vcycle(hierarchy(closed)),64,1e-8);
  assert.ok(result.relativeResidual<1e-8&&result.iterations<32);
  const difference=result.p.map((v,i)=>v-exact[i]),gauge=difference.reduce((a,b)=>a+b,0)/difference.length;
  assert.ok(difference.every(v=>Math.abs(v-gauge)<1e-6));
});
test('impact-scale pressure needs a precise accumulator and residual, not more FP32 iterations',()=>{
  const pressure=[366020.0625,366010.103,365980.127,366090.211,366001.113,366018.176];
  const coefficients=[5,-1,-1,-1,-1,-1];
  const exact=pressure.reduce((s,p,i)=>s+coefficients[i]*p,0),rhs=Math.fround(exact);
  let fp32=0;
  pressure.forEach((p,i)=>fp32=Math.fround(fp32+Math.fround(coefficients[i]*Math.fround(p))));
  const scale=1/(998.207*.08*.08*120);
  assert.ok(Math.abs(rhs-fp32)*scale>1e-4,'Fixture must expose the physical FP32 residual floor');
  assert.ok(Math.abs(rhs-exact)*scale<1e-7);
  let coarse=366020.0625,precise=coarse;
  for(let i=0;i<1000;i++){coarse=Math.fround(coarse+.0001);precise+=.0001;}
  assert.equal(coarse,366020.0625);
  assert.ok(Math.abs(precise-366020.1625)<1e-7);
});
