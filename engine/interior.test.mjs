import test from 'node:test';
import assert from 'node:assert/strict';
const add=(a,b)=>a.map((x,i)=>x+b[i]),sub=(a,b)=>a.map((x,i)=>x-b[i]);
const scale=(v,s)=>v.map(x=>x*s),dot=(a,b)=>a.reduce((s,x,i)=>s+x*b[i],0);
const mv=(C,x)=>C.map(r=>dot(r,x));
const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
const close=(a,b,e=2e-10)=>assert.ok(Math.abs(a-b)<e,`${a} != ${b}`);
let seed=741;const random=()=>((seed=(Math.imul(seed,1664525)+1013904223)>>>0)/2**32);
const vec=()=>Array.from({length:3},()=>random()*2-1);
const kernel=x=>(x=Math.abs(x))<.5?.75-x*x:x<1.5?.5*(1.5-x)**2:0;
const nodes=(n,s)=>Array.from({length:n[0]*n[1]*n[2]},(_,i)=>[i%n[0],Math.floor(i/n[0])%n[1],Math.floor(i/(n[0]*n[1]))].map((x,a)=>(x-(n[a]-1)/2)*s[a]));
const measure=(q,n,s,h)=>n.reduce((p,extent,a)=>p*Array.from({length:extent},(_,i)=>kernel(q[a]-(i-(extent-1)/2)*s[a]/h)).reduce((a,b)=>a+b,0),1);
function totals(p,D){const out={mass:0,linear:[0,0,0],angular:[0,0,0],energy:0};
  for(const q of p){out.mass+=q.m;out.linear=add(out.linear,scale(q.v,q.m));
    const spin=[D[1]*q.C[2][1]-D[2]*q.C[1][2],D[2]*q.C[0][2]-D[0]*q.C[2][0],D[0]*q.C[1][0]-D[1]*q.C[0][1]];
    out.angular=add(out.angular,scale(add(cross(q.x,q.v),spin),q.m));
    out.energy+=.5*q.m*(dot(q.v,q.v)+q.C.reduce((sum,row)=>sum+dot(row.map(x=>x*x),D),0));
  }return out;}
test('separable dormant density reproduces explicit anisotropic lattice P2G weights',()=>{
  for(let i=0;i<1200;++i){const h=.08,n=[4+i%2,4+(i>>1)%2,4+(i>>2)%2],s=[.036,.039,.042],q=scale(vec(),3);
    const explicit=nodes(n,s).reduce((v,d)=>v+d.reduce((w,x,a)=>w*kernel(q[a]-x/h),1),0);
    close(explicit,measure(q,n,s,h));
  }
});
test('coarse lattice moment matches explicit APIC mass, momentum and energy',()=>{
  const h=.08,D=[h*h/4,h*h/4,h*h/4];
  for(let i=0;i<500;i++){const n=[4+i%2,4+(i>>1)%2,4+(i>>2)%2],s=[.036,.039,.042],x=vec(),v=vec(),C=[vec(),vec(),vec()];
    const offsets=nodes(n,s),M=offsets.length,moment=D.map((d,a)=>d+s[a]**2*(n[a]**2-1)/12);
    const a=totals([{m:M,x,v,C}],moment),b=totals(offsets.map(d=>({m:1,x:add(x,d),v:add(v,mv(C,d)),C})),D);
    close(a.mass,b.mass);close(a.energy,b.energy);
    for(const key of ['linear','angular'])for(let k=0;k<3;k++)close(a[key][k],b[key][k]);
  }
});
test('separable MAC transpose reproduces constant and affine fields',()=>{
  const h=.08,n=[4,5,4],s=[.04,.032,.04],count=n.reduce((a,b)=>a*b),moment=n.map((m,a)=>h*h/4+s[a]**2*(m*m-1)/12);
  for(let k=0;k<20;k++){const gp=scale(vec(),.25),base=vec(),C=[vec(),vec(),vec()];
    for(let axis=0;axis<3;axis++){let mass=0,v=0,B=[0,0,0];
      for(let z=-3;z<=3;z++)for(let y=-3;y<=3;y++)for(let x=-3;x<=3;x++){
        const q=sub([x,y,z],gp),w=measure(q,n,s,h)/count,u=base[axis]+dot(C[axis],scale(q,h));
        mass+=w;v+=w*u;B=add(B,scale(q,w*u*h));
      }close(mass,1);close(v,base[axis]);for(let a=0;a<3;a++)close(B[a]/moment[a],C[axis][a]);
    }
  }
});
test('coarse block and requested brick mapping includes the two-cell render padding',()=>{
  for(let owner=0;owner<24;owner++){const b=Math.floor((owner*2+2)/4);
    for(let child=0;child<2;child++)assert.equal(Math.floor((owner*2+child+2)/4),b);
  }
});
