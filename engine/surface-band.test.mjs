import test from 'node:test';
import assert from 'node:assert/strict';

const key=p=>p.join(',');
function crossings(field,n=24){
  const cells=[];
  for(let z=0;z<n;z++)for(let y=0;y<n;y++)for(let x=0;x<n;x++){
    const values=Array.from({length:8},(_,k)=>field(x+(k&1),y+((k>>1)&1),z+(k>>2)));
    if(Math.min(...values)<=0&&Math.max(...values)>=0)cells.push([x,y,z]);
  }
  return cells;
}
function neighborhood(center,cells){
  const masks=new Uint32Array(432);
  for(const p of cells){
    const b=p.map((x,a)=>Math.floor(x/8)-center[a]+1);
    if(b.some(x=>x<0||x>2))continue;
    const brick=(b[2]*3+b[1])*3+b[0],cell=(p[2]%8)*64+(p[1]%8)*8+p[0]%8;
    masks[brick*16+(cell>>>5)]|=1<<(cell&31);
  }
  return masks;
}
function packedBand(masks,node){
  const lo=node.map(x=>x+6),hi=node.map(x=>x+9);
  for(let z=lo[2];z<=hi[2];z++)for(let y=lo[1];y<=hi[1];y++)
    for(let bx=Math.floor(lo[0]/8);bx<=Math.floor(hi[0]/8);bx++){
      const first=Math.max(lo[0],bx*8)-bx*8,last=Math.min(hi[0],bx*8+7)-bx*8;
      const neighbor=(Math.floor(z/8)*3+Math.floor(y/8))*3+bx,row=(z%8)*64+(y%8)*8;
      const bits=((1<<(last-first+1))-1)<<(row%32+first);
      if(masks[neighbor*16+Math.floor(row/32)]&bits)return true;
    }
  return false;
}
function referenceBand(cells){
  const nodes=new Set();
  for(const p of cells)for(let z=-1;z<=2;z++)for(let y=-1;y<=2;y++)for(let x=-1;x<=2;x++)
    nodes.add(key([p[0]+x,p[1]+y,p[2]+z]));
  return nodes;
}
test('packed interface neighborhoods match independent world-node dilation at brick faces, edges and corners',()=>{
  for(const field of [(x,y,z)=>y-8.37,(x,y,z)=>x+y+z-20.2,
    (x,y,z)=>Math.hypot(x-8,y-8,z-8)-3.4,(x,y,z)=>Math.abs(y-8)-.01,
    (x,y,z)=>(y-8)**2,(x,y,z)=>-1+.03*Math.sin(x+y+z)]){
    const cells=crossings(field),band=referenceBand(cells);
    for(const b of [[0,0,0],[1,1,1],[0,1,2],[2,2,2]]){
      const masks=neighborhood(b,cells);
      for(let z=0;z<=8;z++)for(let y=0;y<=8;y++)for(let x=0;x<=8;x++){
        const p=[x,y,z];
        assert.equal(packedBand(masks,p),band.has(key(p.map((v,a)=>v+b[a]*8))),`${b} / ${p}`);
      }
    }
  }
});
test('negative interior scalar ripples are not surfaces while tangencies and thin sheets remain protected',()=>{
  assert.equal(crossings((x,y,z)=>-1+.03*Math.sin(x+y+z)).length,0);
  const sheet=referenceBand(crossings((x,y,z)=>Math.abs(y-8)-.01));
  const tangent=referenceBand(crossings((x,y,z)=>(y-8)**2));
  for(let y=6;y<=10;y++){
    assert.ok(sheet.has(key([8,y,8])));assert.ok(tangent.has(key([8,y,8])));
  }
});
test('the geometric band contains all nonzero trilinear supports of photon normal differentials',()=>{
  const c=[8,8,8],band=referenceBand([c]);
  for(const fraction of [.001,.23,.71,.999])for(let axis=0;axis<3;axis++)for(const s of [-.5,.5])
    for(let normalAxis=0;normalAxis<3;normalAxis++)for(const t of [-.5,.5]){
      const p=c.map(x=>x+fraction);p[axis]+=s;p[normalAxis]+=t;
      const base=p.map(Math.floor);
      for(let k=0;k<8;k++)assert.ok(band.has(key(base.map((x,a)=>x+((k>>a)&1)))));
    }
});
