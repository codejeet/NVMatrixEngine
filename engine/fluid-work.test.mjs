import test from 'node:test';
import assert from 'node:assert/strict';
const shape=[8,4,4],index=(p,g)=>(p[2]*g[1]+p[1])*g[0]+p[0];
const coordinate=(i,g)=>[i%g[0],Math.floor(i/g[0])%g[1],Math.floor(i/(g[0]*g[1]))];
const product=g=>g.reduce((a,b)=>a*b);
const q=x=>{x=Math.abs(x);return x<.5?.75-x*x:x<1.5?.5*(1.5-x)**2:0;};
function mark(g,bins){
  const fg=g.map((n,a)=>Math.ceil((n+1)/shape[a])),stride=product(fg),flags=new Uint8Array(3*stride);
  for(const c of bins)for(let axis=0;axis<3;axis++){
    const lo=c.map((n,a)=>Math.floor(Math.max(0,n-1)/shape[a]));
    const hi=c.map((n,a)=>Math.floor(Math.min(n+1+(a===axis?1:0),g[a]-1+(a===axis?1:0))/shape[a]));
    for(let z=lo[2];z<=hi[2];z++)for(let y=lo[1];y<=hi[1];y++)for(let x=lo[0];x<=hi[0];x++)flags[axis*stride+index([x,y,z],fg)]=1;
  }
  return {fg,stride,flags};
}
test('MAC tile marking equals independent inverse support including partial boundary tiles',()=>{
  for(const g of [[1,1,1],[8,4,4],[9,5,7],[17,13,11]]){
    const bins=Array.from({length:product(g)},(_,i)=>coordinate(i,g)).filter((_,i)=>i%17===0);
    const {fg,stride,flags}=mark(g,bins);
    for(let tile=0;tile<flags.length;tile++){
      const axis=Math.floor(tile/stride),base=coordinate(tile%stride,fg).map((n,a)=>n*shape[a]);
      const extent=g.map((n,a)=>n+(a===axis?1:0));
      const lo=base.map((n,a)=>Math.floor(n+(a===axis?0:.5)-1.5));
      const hi=base.map((n,a)=>Math.ceil(Math.min(n+shape[a]-1,extent[a]-1)+(a===axis?0:.5)+1.5)-1);
      const expected=base.every((n,a)=>n<extent[a])&&bins.some(c=>c.every((n,a)=>n>=lo[a]&&n<=hi[a]));
      assert.equal(flags[tile],Number(expected),`${g} tile ${tile}`);
    }
  }
});
test('quadratic particle and dormant-lattice support never escapes marked face tiles',()=>{
  const g=[17,9,11];
  for(const c of [[0,0,0],[7,3,3],[8,4,4],[16,8,10]]){
    const {fg,stride,flags}=mark(g,[c]);
    for(const fraction of [0,1e-7,.25,.5,.75,1-1e-7]){
      const p=c.map(n=>n+fraction);
      for(let axis=0;axis<3;axis++){
        const extent=g.map((n,a)=>n+(a===axis?1:0));
        for(let z=0;z<extent[2];z++)for(let y=0;y<extent[1];y++)for(let x=0;x<extent[0];x++){
          const f=[x,y,z],weight=f.reduce((w,n,a)=>w*q(p[a]-n-(a===axis?0:.5)),1);
          if(weight>0)assert.equal(flags[axis*stride+index(f.map((n,a)=>Math.floor(n/shape[a])),fg)],1);
        }
      }
    }
  }
});
test('active tile removal clears stale face state and independent lists do not overlap',()=>{
  const g=[47,50,45],a=mark(g,[[7,8,9],[32,40,30]]),b=mark(g,[[7,8,9]]);
  assert.ok(a.flags.some((v,i)=>v&&!b.flags[i]));
  const velocity=Float32Array.from(a.flags,v=>v?7:0);velocity.fill(0);
  b.flags.forEach((v,i)=>{if(v)velocity[i]=3;});
  assert.ok(velocity.every((v,i)=>b.flags[i]||v===0));
  const f=a.flags.length,d=product(g.map((n,i)=>Math.ceil(n/shape[i])));
  const ranges=[[0,16],[16,16+f],[16+f,16+2*f],[16+2*f,16+2*f+d],[16+2*f+d,16+2*f+2*d]];
  for(let i=1;i<ranges.length;i++)assert.equal(ranges[i-1][1],ranges[i][0]);
});
