import test from 'node:test';
import assert from 'node:assert/strict';
import {macMesh,dot,multiply} from './adaptive-mac-reference.mjs';
import {hierarchy,vcycle,pcg} from './mac-multigrid-reference.mjs';

function weightedMesh(){
  const m=macMesh([12,8,12],(x,y,z)=>y>1&&y<3&&(x+z)%3===0);
  m.rows=m.nodes.map(()=>new Map());
  for(const f of m.faces){
    // Partial cut faces only in the protected fine band; T patches stay full.
    const weight=f.width===1?.02+.98*Math.abs(Math.sin(dot(f.center,[.33,.17,.23]))):1;
    f.volume*=weight;f.aperture=weight;
    for(const [i,a] of f.gradient)for(const [j,b] of f.gradient)
      m.rows[i].set(j,(m.rows[i].get(j)??0)+f.volume*a*b);
  }
  for(const n of m.nodes){
    n.volume*=n.width===1?.04+.96*Math.abs(Math.sin(n.id*.71)):1;
    n.boundary=n.base[1]===0?.13+.77*Math.abs(Math.sin(n.id*.17)):0;
    m.rows[n.id].set(n.id,m.rows[n.id].get(n.id)+n.boundary);
  }
  return m;
}
test('fractional face weights retain an adjoint flux/gradient and SPD pressure operator',()=>{
  const m=weightedMesh(),p=m.nodes.map(n=>Math.sin(n.id*.71)),u=m.faces.map((_,i)=>Math.cos(i*.33)),div=p.map(()=>0);
  let work=0;
  m.faces.forEach((f,k)=>{
    for(const [i,d]of f.divergence)div[i]+=f.aperture*d*u[k];
    work+=f.volume*u[k]*[...f.gradient].reduce((s,[i,g])=>s+g*p[i],0);
  });
  assert.ok(Math.abs(dot(p,div)+work)<1e-10);
  for(let i=0;i<m.rows.length;i++){
    for(const[j,a]of m.rows[i])assert.ok(Math.abs(a-(m.rows[j].get(i)??0))<1e-12);
    assert.ok(Math.abs([...m.rows[i].values()].reduce((s,x)=>s+x,0)-m.nodes[i].boundary)<1e-12);
  }
  assert.ok(dot(p,multiply(m.rows,p))>0);
});
test('fractional Dirichlet conductances survive Galerkin aggregation and MGPCG',()=>{
  const m=weightedMesh(),levels=hierarchy(m);
  for(let l=0;l<levels.length-1;l++){
    const a=levels[l],b=levels[l+1],p=b.nodes.map((_,i)=>Math.sin(i*.18));
    assert.ok(Math.abs(dot(p,multiply(b.rows,p))-dot(a.parent.map(i=>p[i]),multiply(a.rows,a.parent.map(i=>p[i]))))<1e-9);
  }
  const exact=m.nodes.map(n=>Math.sin(n.center[0]*.1)+.2*n.center[1]),rhs=multiply(m.rows,exact);
  const result=pcg(m.rows,rhs,vcycle(levels),128,1e-9);
  assert.ok(result.relativeResidual<1e-9);
  assert.ok(Math.max(...result.p.map((p,i)=>Math.abs(p-exact[i])))<1e-6);
});
test('swept volume is a pressure flux source with actual open-volume normalization',()=>{
  const h=.16,dt=1/120,rho=998.207,area=h*h,velocity=.7;
  const oldVolume=.7*h**3,newVolume=oldVolume-velocity*dt*area;
  const source=(newVolume-oldVolume)/dt;
  assert.ok(Math.abs(source+velocity*area)<1e-15);
  // Single open outlet, fixed air pressure, moving wall on the opposite side.
  const a=area/h**2,rhs=-rho*source/(dt*h),pressure=rhs/a;
  const outlet=dt/(rho*h)*pressure;
  assert.ok(Math.abs(outlet-velocity)<1e-12);
  assert.ok(Math.abs(outlet*area+source)<1e-15);
  // Physical divergence depends on actual open volume, never an assumed 1/8.
  const residual=.1,full=dt/(rho*h*h)*residual,partial=dt*h/(rho*newVolume)*residual;
  assert.ok(partial>full);
});
test('sliver pressure targets retain precision before the final FP32 velocity cache',()=>{
  // Observed moving-solid scale: an FP32 RHS can meet its own residual target
  // while missing the actual face-flux equation by several 1e-4 / second.
  const rhs=12465.85591,h=.08,dt=1/120,rho=998.207,volume=.001016;
  const scale=dt/(rho*h*h*volume),coefficient=.082026;
  const roundedPressure=Math.fround(rhs)/coefficient,exactPressure=rhs/coefficient;
  assert.ok(Math.abs(rhs-coefficient*roundedPressure)*scale>1e-4);
  assert.ok(Math.abs(rhs-coefficient*exactPressure)*scale<1e-8);
});
test('partial solid support and liquid capacity partition the density kernel',()=>{
  for(const fraction of [0,.001,.25,.5,.9,1]){
    let liquid=0,solid=0;
    for(let z=-1;z<=1;z++)for(let y=-1;y<=1;y++)for(let x=-1;x<=1;x++){
      const w=[x,y,z].reduce((p,v)=>p*(v===0?2/3:1/6),1);
      const open=x<0?0:x===0?fraction:1;
      liquid+=open*w;solid+=(1-open)*w;
    }
    assert.ok(Math.abs(liquid+solid-1)<1e-14);
    assert.ok(Math.abs(liquid-(1/6+fraction*2/3))<1e-14);
  }
});
