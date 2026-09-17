import {readFileSync} from 'node:fs';
import assert from 'node:assert/strict';
const data=readFileSync(process.argv[2]==='-'?0:process.argv[2]);
const u=offset=>data.readUInt32LE(offset),f=offset=>data.readFloatLE(offset);
assert.equal(u(0),0x3150474d);assert.ok([1,2,3].includes(u(4)));assert.equal(u(16),304);
const version=u(4),pressureBytes=version>=2?8:4,rowBytes=version>=3?216:208,cut=version>=3&&u(28)!==0;
const step=Number(data.readBigUInt64LE(8)),capacity=u(20),iterations=u(24),g=[u(32),u(36),u(40),u(44)];
const scale=f(32+296),rowStart=336,mapStart=rowStart+rowBytes*g[3],cellStart=mapStart+4*g[3],pressureStart=cellStart+16*g[3];
const coarseStart=pressureStart+pressureBytes*g[3],factorStart=coarseStart+32*capacity,exactStart=factorStart+16384,scalarStart=exactStart+(cut?64*g[3]:0);
const pressure=id=>pressureBytes===8?data.readDoubleLE(pressureStart+id*8):f(pressureStart+id*4);
assert.equal(data.length,scalarStart+(4+iterations)*16);
const coord=id=>[id%g[0],Math.floor(id/g[0])%g[1],Math.floor(id/(g[0]*g[1]))];
const rows=[],byId=new Map(),worst=[];
for(let id=0;id<g[3];id++){
  if(f(cellStart+id*16+8)!==1||u(mapStart+id*4)!==id)continue;
  const at=rowStart+rowBytes*id,count=u(at),diagonal=f(at+4),rhs=cut?data.readDoubleLE(exactStart+id*64):f(at+8),p=pressure(id),neighbors=[];
  assert.ok(count<=24);let ap=diagonal*p,rowSum=diagonal;
  for(let j=0;j<count;j++){
    const other=u(at+16+j*4),a=f(at+112+j*4);assert.ok(other<g[3]);neighbors.push([other,a]);
    ap+=a*pressure(other);rowSum+=a;
  }
  const c=coord(id),big=c.every((v,a)=>v+1<g[a])&&u(mapStart+4*(id+1+g[0]+g[0]*g[1]))===id;
  if(cut&&data.readDoubleLE(exactStart+id*64+8)>=0){
    ap=0;
    for(let a=0;a<3;a++)for(let s=0;s<2;s++){
      const w=data.readDoubleLE(exactStart+id*64+16+(a*2+s)*8),q=c.slice();q[a]+=s?1:-1;
      const n=(q[2]*g[1]+q[1])*g[0]+q[0],inside=q.every((v,i)=>v>=0&&v<g[i]);
      ap+=w*(p-(inside&&f(cellStart+n*16+8)===1?pressure(n):0));
    }
  }
  const volume=version>=3?f(at+212):(big?8:1);
  const r={id,coord:c,diagonal,rhs,p,volumeUnits:volume,residual:rhs-ap,divergence:Math.abs(rhs-ap)*scale/volume,rowSum,neighbors};
  byId.set(id,rows.length);rows.push(r);worst.push(r);
}
const seen=new Set(),components=[];
for(const start of rows){
  if(seen.has(start.id))continue;const ids=[start.id];seen.add(start.id);let rhs=0,air=0,peak=0;
  for(let i=0;i<ids.length;i++){
    const r=rows[byId.get(ids[i])];rhs+=r.rhs;air+=r.rowSum;peak=Math.max(peak,r.divergence);
    for(const [other,a] of r.neighbors)if(a&& !seen.has(other)){assert.ok(byId.has(other));seen.add(other);ids.push(other);}
  }
  components.push({count:ids.length,rhsSum:rhs,airDiagonal:air,peakDivergence:peak});
}
worst.sort((a,b)=>b.divergence-a.divergence);
const trace=Array.from({length:iterations},(_,i)=>{const at=scalarStart+(4+i)*16;return {iteration:i+1,normSquared:f(at),maxDivergence:f(at+4),rz:f(at+8),alpha:f(at+12)};});
console.log(JSON.stringify({version,cutPressure:cut,step,grid:g,pressureScale:scale,active:rows.length,components,worst:worst.slice(0,12),trace},null,2));
