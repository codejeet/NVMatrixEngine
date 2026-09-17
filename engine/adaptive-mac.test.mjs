import test from 'node:test';
import assert from 'node:assert/strict';
import {macMesh,dot,multiply,divergence,project,prolongateInterior} from './adaptive-mac-reference.mjs';
const mesh=macMesh([10,8,10],(x,y,z)=>(x+y+z)%3!==0&&x>0&&y>0&&z>0);
test('mixed MAC topology covers volume once with shared fine/coarse face fluxes',()=>{
  assert.equal(mesh.nodes.reduce((s,n)=>s+n.volume,0),800);
  assert.ok(mesh.nodes.some(n=>n.width===1)&&mesh.nodes.some(n=>n.width===2));
  let junctions=0;
  for(const f of mesh.faces){assert.equal([...f.divergence.values()].reduce((s,x)=>s+x,0),0);
    if(f.gradient.size===5){junctions++;assert.equal(f.volume,6);assert.equal(f.area,4);}}
  assert.ok(junctions>10,'Exercise real five-pressure T junctions, not a coarse correction replica');
});
test('T-junction pressure gradients reproduce arbitrary affine pressure including hydrostatics',()=>{
  const gradient=[.3,-9.81,.7],p=mesh.nodes.map(n=>2+dot(n.center,gradient));
  for(const f of mesh.faces){const g=[...f.gradient].reduce((s,[i,w])=>s+p[i]*w,0);
    assert.ok(Math.abs(g-gradient[f.axis])<1e-12,'No tangential-pressure leakage into normal gradient');}
});
test('adaptive gradient/divergence are adjoints and the pressure operator is symmetric positive semidefinite',()=>{
  const p=mesh.nodes.map(n=>Math.sin(n.id*.73)),v=mesh.faces.map((_,i)=>Math.cos(i*.31));
  const dp=dot(p,divergence(mesh,v));let gp=0;
  for(let k=0;k<mesh.faces.length;k++){const f=mesh.faces[k];gp+=f.volume*v[k]*[...f.gradient].reduce((s,[i,g])=>s+g*p[i],0);}
  assert.ok(Math.abs(dp+gp)<1e-10);
  mesh.rows.forEach((r,i)=>{for(const [j,a] of r)assert.ok(Math.abs(a-(mesh.rows[j].get(i)??0))<1e-13);});
  assert.ok(dot(p,multiply(mesh.rows,p))>0);
  assert.ok(multiply(mesh.rows,p.map(()=>1)).every(x=>Math.abs(x)<1e-12));
});
test('mixed-resolution projection removes divergence without adding kinetic energy',()=>{
  const velocity=mesh.faces.map((_,i)=>Math.sin(i*.19)+.3*Math.cos(i*.73)),result=project(mesh,velocity);
  const energy=v=>mesh.faces.reduce((s,f,k)=>s+.5*f.volume*v[k]**2,0);
  assert.ok(energy(result.velocity)<=energy(velocity));
  assert.ok(Math.max(...divergence(mesh,result.velocity).map(Math.abs))<1e-9);
  assert.ok(result.iterations<300);
});
test('coarse-to-fine internal reconstruction preserves external flux and distributes divergence continuously',()=>{
  for(let trial=0;trial<100;trial++){
    const faces=Array.from({length:3},(_,a)=>Array.from({length:2},(_,s)=>Array.from({length:4},(_,j)=>Math.sin((trial+1)*(a+1)+s*.5+j*.3))));
    const copy=JSON.stringify(faces),r=prolongateInterior(faces);
    assert.equal(JSON.stringify(faces),copy);
    assert.ok(r.after.every(x=>Math.abs(x-r.mean)<1e-12));
    assert.ok(Math.abs(r.after.reduce((s,x)=>s+x,0)-r.before.reduce((s,x)=>s+x,0))<1e-12);
  }
});
test('GPU row capacity and damped Jacobi bound cover mixed positive off-diagonal couplings',()=>{
  for(const row of mesh.rows)assert.ok(row.size<=25);
  const b=mesh.nodes.map(n=>Math.sin(n.id*.3)),p=b.map(()=>0);
  // The objective is a convex quadratic on zero-mean RHS. Every bounded
  // relaxation must reduce it, including before convergence.
  const mean=b.reduce((s,x)=>s+x,0)/b.length;b.forEach((_,i)=>b[i]-=mean);
  const objective=x=>.5*dot(x,multiply(mesh.rows,x))-dot(x,b);
  let last=objective(p);
  for(let i=0;i<360;i++){
    const ap=multiply(mesh.rows,p);
    for(let j=0;j<p.length;j++)p[j]+=(b[j]-ap[j])/(3*mesh.rows[j].get(j));
    const next=objective(p);assert.ok(next<=last+1e-10);last=next;
  }
});
test('absolute-row-sum relaxation remains energy decreasing without globally damping fine cells by three',()=>{
  const b=mesh.nodes.map(n=>Math.sin(n.id*.3)),p=b.map(()=>0);
  const mean=b.reduce((s,x)=>s+x,0)/b.length;b.forEach((_,i)=>b[i]-=mean);
  const steps=mesh.rows.map(r=>1.8/[...r.values()].reduce((s,v)=>s+Math.abs(v),0));
  const objective=x=>.5*dot(x,multiply(mesh.rows,x))-dot(x,b);
  let last=objective(p);
  for(let i=0;i<120;i++){
    const ap=multiply(mesh.rows,p);
    for(let j=0;j<p.length;j++)p[j]+=(b[j]-ap[j])*steps[j];
    const next=objective(p);assert.ok(next<=last+1e-10);last=next;
  }
  // H=diag(sum_j |Aij|); Gershgorin bounds lambda(H^-1 A)<=1.
  // A is PSD, so 0 < omega=1.8 < 2 is a safe Richardson step.
  for(let i=0;i<steps.length;i++)assert.ok(steps[i]*[...mesh.rows[i].values()].reduce((s,v)=>s+Math.abs(v),0)<2);
});
