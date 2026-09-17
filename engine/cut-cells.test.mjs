import test from 'node:test';
import assert from 'node:assert/strict';

const edge=(a,b)=>a/(a-b);
function tetra(phi){
  const p=phi.filter(x=>x>=0),n=phi.filter(x=>x<0);
  if(!p.length)return 0;if(!n.length)return 1;
  if(p.length===1)return n.reduce((v,x)=>v*edge(p[0],x),1);
  if(n.length===1)return 1-p.reduce((v,x)=>v*edge(-n[0],-x),1);
  const a=edge(p[0],n[0]),b=edge(p[0],n[1]),c=edge(p[1],n[0]),d=edge(p[1],n[1]);
  return a*b+a*d*(1-b)+c*d*(1-a);
}
function triangle(phi){
  const p=phi.filter(x=>x>=0),n=phi.filter(x=>x<0);
  if(!p.length)return 0;if(!n.length)return 1;
  return p.length===1?edge(p[0],n[0])*edge(p[0],n[1]):1-edge(-n[0],-p[0])*edge(-n[0],-p[1]);
}
const tets=[[0,1,3,7],[0,1,5,7],[0,2,3,7],[0,2,6,7],[0,4,5,7],[0,4,6,7]];
const xyz=i=>[i&1,(i>>1)&1,i>>2];
const cube=phi=>tets.reduce((v,t)=>v+tetra(t.map(i=>phi[i]))/6,0);
const near=(a,b,e=1e-12)=>assert.ok(Math.abs(a-b)<=e,`${a} != ${b}`);
function* permutations(a){if(!a.length){yield [];return;}for(let i=0;i<a.length;i++)for(const tail of permutations(a.filter((_,j)=>i!==j)))yield [a[i],...tail];}

test('cut simplex is bounded, complementary and permutation invariant, including coincident vertices',()=>{
  for(const p of [[-1,-1,1,1],[-3,.1,.8,2],[-.7,-2,-.01,9],[0,-1,2,0],[0,0,0,-1],[1e-20,-2e-20,3e-20,-4e-20]]){
    const f=tetra(p);assert.ok(f>=0&&f<=1);near(f+tetra(p.map(x=>-x)),1);
    for(const q of permutations(p))near(tetra(q),f);
  }
  near(tetra([0,0,0,0]),1);near(triangle([0,0,0]),1);
});
test('cut cube integrates arbitrary translated planar slabs exactly',()=>{
  for(let axis=0;axis<3;axis++)for(let k=-5;k<=25;k++){
    const t=k/20,phi=Array.from({length:8},(_,i)=>xyz(i)[axis]-t);
    near(cube(phi),Math.max(0,Math.min(1,1-t)));
  }
  // Independent CDF of a sum of three uniform variables (inclusion/exclusion).
  for(const weights of [[1,1,1],[.3,.7,1.4],[2,.1,.7]])for(let k=1;k<40;k++){
    const offset=k/40*weights.reduce((a,b)=>a+b),phi=Array.from({length:8},(_,i)=>xyz(i).reduce((s,x,a)=>s+x*weights[a],-offset));
    let cdf=0;for(let mask=0;mask<8;mask++){
      const bits=xyz(mask),s=bits.reduce((s,x,a)=>s+x*weights[a],0);
      cdf+=(-1)**bits.reduce((a,b)=>a+b)*Math.max(0,offset-s)**3;
    }
    near(cube(phi),1-cdf/(6*weights.reduce((a,b)=>a*b)),2e-12);
  }
});
test('shared face diagonals match cell tetrahedra on all six faces',()=>{
  for(let a=0;a<3;a++)for(let side=0;side<2;side++){
    const boundary=tets.flatMap(t=>t.map((_,i)=>t.filter((_,j)=>j!==i))).filter(t=>t.every(i=>xyz(i)[a]===side));
    const b=(a+1)%3,c=(a+2)%3;
    const id=(u,v)=>(side<<a)|(u<<b)|(v<<c);
    const key=t=>t.toSorted((a,b)=>a-b).join(',');
    assert.deepEqual(boundary.map(key).sort(),[[id(0,0),id(1,0),id(1,1)],[id(0,0),id(0,1),id(1,1)]].map(key).sort());
  }
});
test('fine to coarse capacities are summed, not re-sampled; partial last cells conserve domain measure',()=>{
  const lengths=[2.31,1.79,3.14],h=.4,n=lengths.map(x=>Math.ceil(x/h)),coarse=n.map(x=>Math.ceil(x/2));
  let fineSum=0,coarseSum=0;const values=new Map();
  for(let z=0;z<n[2];z++)for(let y=0;y<n[1];y++)for(let x=0;x<n[0];x++){
    const p=[x,y,z],w=p.map((v,a)=>Math.min(h,lengths[a]-v*h));
    const phi=Array.from({length:8},(_,i)=>{const q=xyz(i);return y*h+q[1]*w[1]-.73;});
    const v=cube(phi)*w.reduce((a,b)=>a*b);fineSum+=v;values.set(p.join(','),v);
  }
  for(let z=0;z<coarse[2];z++)for(let y=0;y<coarse[1];y++)for(let x=0;x<coarse[0];x++)for(let i=0;i<8;i++){
    const q=xyz(i).map((v,a)=>v+2*[x,y,z][a]);coarseSum+=values.get(q.join(','))??0;
  }
  near(fineSum,coarseSum);near(fineSum,lengths[0]*(lengths[1]-.73)*lengths[2]);
});
