import {dot,multiply} from './adaptive-mac-reference.mjs';

// Constant geometric aggregation of the actual mixed operator. This reference
// keeps general sparse rows; it does not assume the GPU's compact six-face form.
export function hierarchy(mesh){
  const levels=[{nodes:mesh.nodes,rows:mesh.rows}];
  let spacing=4;
  while(levels.at(-1).nodes.length>64){
    const fine=levels.at(-1),nodes=[],map=[],ids=new Map();
    for(const n of fine.nodes){
      const base=n.base.map(x=>Math.floor(x/spacing)*spacing),key=base.join(',');
      if(!ids.has(key)){ids.set(key,nodes.length);nodes.push({id:nodes.length,base,width:spacing,center:base.map(x=>x+spacing/2)});}
      map.push(ids.get(key));
    }
    const rows=nodes.map(()=>new Map());
    fine.rows.forEach((r,i)=>{for(const [j,a] of r){const row=rows[map[i]];row.set(map[j],(row.get(map[j])??0)+a);}});
    fine.parent=map;levels.push({nodes,rows});spacing*=2;
  }
  return levels;
}
export function cholesky(rows,regularization=1e-4){
  const n=rows.length,scale=Math.max(...rows.map((r,i)=>r.get(i)??0),1),l=Array.from({length:n},()=>new Float64Array(n));
  for(let i=0;i<n;i++)for(let j=0;j<=i;j++){
    let a=(rows[i].get(j)??0)+(i===j?regularization*scale:0);
    for(let k=0;k<j;k++)a-=l[i][k]*l[j][k];
    if(i===j){if(!(a>0))throw new Error('Coarse matrix is not SPD');l[i][j]=Math.sqrt(a);}
    else l[i][j]=a/l[j][j];
  }
  return b=>{
    const y=new Float64Array(n),x=new Float64Array(n);
    for(let i=0;i<n;i++){let v=b[i];for(let j=0;j<i;j++)v-=l[i][j]*y[j];y[i]=v/l[i][i];}
    for(let i=n-1;i>=0;i--){let v=y[i];for(let j=i+1;j<n;j++)v-=l[j][i]*x[j];x[i]=v/l[i][i];}
    return Array.from(x);
  };
}
export function vcycle(levels,smoothing=2){
  const bottom=cholesky(levels.at(-1).rows);
  function apply(l,b){
    if(l===levels.length-1)return bottom(b);
    const {rows,parent}=levels[l],p=b.map(()=>0);
    const step=rows.map((r,i)=>l===0?1.44/[...r.values()].reduce((s,a)=>s+Math.abs(a),0):(2/3)/(r.get(i)??1));
    const smooth=()=>{const ap=multiply(rows,p);for(let i=0;i<p.length;i++)p[i]+=(b[i]-ap[i])*step[i];};
    for(let i=0;i<smoothing;i++)smooth();
    const ap=multiply(rows,p),rhs=levels[l+1].nodes.map(()=>0);
    b.forEach((v,i)=>rhs[parent[i]]+=v-ap[i]);
    const coarse=apply(l+1,rhs);p.forEach((_,i)=>p[i]+=coarse[parent[i]]);
    for(let i=0;i<smoothing;i++)smooth();
    return p;
  }
  return b=>apply(0,b);
}
export function pcg(rows,b,precondition,maxIterations=64,tolerance=1e-8){
  const p=b.map(()=>0);let r=[...b],d=b.map(()=>0),old=1,iterations=0;
  const target=dot(b,b)*tolerance*tolerance;
  for(;iterations<maxIterations&&dot(r,r)>target;iterations++){
    const z=precondition(r),rz=dot(r,z),beta=iterations?rz/old:0;old=rz;
    d=d.map((v,i)=>z[i]+beta*v);
    const ad=multiply(rows,d),alpha=rz/dot(d,ad);
    p.forEach((_,i)=>p[i]+=alpha*d[i]);
    const ap=multiply(rows,p);r=b.map((v,i)=>v-ap[i]);
  }
  return {p,iterations,relativeResidual:Math.sqrt(dot(r,r)/Math.max(dot(b,b),1e-30))};
}
