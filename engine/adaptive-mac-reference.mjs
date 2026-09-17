// Independent double-precision reference for a 2:1 staggered MAC hierarchy.
// Units: h = dt = rho = 1. T faces carry one normal flux at the large-face center.
export function macMesh(dimensions, coarsePolicy) {
  const [nx,ny,nz]=dimensions,index=(x,y,z)=>(z*ny+y)*nx+x;
  const map=new Int32Array(nx*ny*nz).fill(-1),nodes=[];
  for(let z=0;z<nz;z++)for(let y=0;y<ny;y++)for(let x=0;x<nx;x++){
    if(map[index(x,y,z)]>=0)continue;
    const width=!(x%2||y%2||z%2)&&x+1<nx&&y+1<ny&&z+1<nz&&coarsePolicy(x/2,y/2,z/2)?2:1;
    const id=nodes.length,node={id,base:[x,y,z],width,volume:width**3,center:[x+width/2,y+width/2,z+width/2]};
    nodes.push(node);
    for(let k=0;k<width;k++)for(let j=0;j<width;j++)for(let i=0;i<width;i++)map[index(x+i,y+j,z+k)]=id;
  }
  const faces=[],seen=new Set();
  for(let a=0;a<3;a++){
    const b=(a+1)%3,c=(a+2)%3,extent=[...dimensions];extent[a]++;
    for(let z=0;z<extent[2];z++)for(let y=0;y<extent[1];y++)for(let x=0;x<extent[0];x++){
      const p=[x,y,z];if(p[a]===0||p[a]===dimensions[a])continue; // closed exterior
      const l=[...p];l[a]--;
      const left=map[index(...l)],right=map[index(...p)];if(left===right)continue;
      const width=Math.max(nodes[left].width,nodes[right].width),base=[...p];
      if(width===2){base[b]&=~1;base[c]&=~1;}
      const key=[a,...base].join(',');if(seen.has(key))continue;seen.add(key);
      const area=width**2,distance=(nodes[left].width+nodes[right].width)/2;
      const divergence=new Map();
      for(let i=0;i<width;i++)for(let j=0;j<width;j++){
        const q=[...base];q[b]+=i;q[c]+=j;const r=map[index(...q)];q[a]--;const l=map[index(...q)];
        divergence.set(l,(divergence.get(l)??0)+1);divergence.set(r,(divergence.get(r)??0)-1);
      }
      const gradient=new Map([...divergence].map(([i,d])=>[i,-d/(area*distance)]));
      const center=[...base];center[b]+=width/2;center[c]+=width/2;
      faces.push({axis:a,base,width,area,distance,center,divergence,gradient,volume:area*distance});
    }
  }
  const rows=nodes.map(()=>new Map());
  for(const face of faces)for(const [i,a] of face.gradient)for(const [j,b] of face.gradient)
    rows[i].set(j,(rows[i].get(j)??0)+face.volume*a*b);
  return {nodes,faces,rows,map,dimensions};
}
export const dot=(a,b)=>a.reduce((s,x,i)=>s+x*b[i],0);
export const multiply=(rows,x)=>rows.map(row=>[...row].reduce((s,[j,a])=>s+a*x[j],0));
export function divergence(mesh,velocity){
  const d=mesh.nodes.map(()=>0);
  mesh.faces.forEach((f,k)=>{for(const [i,a] of f.divergence)d[i]+=a*velocity[k];});return d;
}
export function project(mesh,velocity){
  const b=divergence(mesh,velocity).map(x=>-x),p=b.map(()=>0),r=[...b];
  let z=r.map((x,i)=>x/mesh.rows[i].get(i)),direction=[...z],rz=dot(r,z),iterations=0;
  for(;iterations<2000&&dot(r,r)>1e-22*Math.max(1,dot(b,b));iterations++){
    const ad=multiply(mesh.rows,direction),alpha=rz/dot(direction,ad);
    for(let i=0;i<p.length;i++){p[i]+=alpha*direction[i];r[i]-=alpha*ad[i];}
    z=r.map((x,i)=>x/mesh.rows[i].get(i));const next=dot(r,z),beta=next/rz;rz=next;
    direction=z.map((x,i)=>x+beta*direction[i]);
  }
  const corrected=mesh.faces.map((f,k)=>velocity[k]-[...f.gradient].reduce((s,[i,g])=>s+g*p[i],0));
  return {p,velocity:corrected,iterations};
}
// External faces are indexed [axis][side][two tangential child bits].
// Return the 12 shared internal fine faces, preserving every external flux.
export function prolongateInterior(external){
  const interior=external.map(sides=>sides[0].map((v,i)=>(v+sides[1][i])*.5));
  const bits=i=>[i&1,(i>>1)&1,(i>>2)&1];
  const divergence=()=>Array.from({length:8},(_,i)=>{
    const p=bits(i);let d=0;
    for(let a=0;a<3;a++){const j=p[(a+1)%3]+2*p[(a+2)%3];
      d+=p[a]?external[a][1][j]-interior[a][j]:interior[a][j]-external[a][0][j];}
    return d;
  });
  const before=divergence(),mean=before.reduce((s,x)=>s+x,0)/8,phi=new Float64Array(8);
  const parity=i=>((i&1)+((i>>1)&1)+((i>>2)&1))%2?-1:1;
  for(let mode=1;mode<8;mode++){
    const eigenvalue=2*((mode&1)+((mode>>1)&1)+((mode>>2)&1));
    const coefficient=before.reduce((s,d,i)=>s+parity(i&mode)*(mean-d),0)/(8*eigenvalue);
    for(let i=0;i<8;i++)phi[i]+=parity(i&mode)*coefficient;
  }
  for(let a=0;a<3;a++)for(let j=0;j<4;j++){
    const p=[0,0,0];p[(a+1)%3]=j&1;p[(a+2)%3]=j>>1;
    const left=p[0]+2*p[1]+4*p[2],right=left+(1<<a);
    interior[a][j]-=phi[right]-phi[left];
  }
  return {interior,before,after:divergence(),mean};
}
