import test from 'node:test';
import assert from 'node:assert/strict';

const cdf=x=>{
  const a=Math.abs(x),tail=a>=1.5?0:a>=.5?(1.5-a)**3/6:.5-.75*a+a**3/3;
  return x<0?tail:1-tail;
};
const integral=(a,b)=>a>=0?cdf(-a)-cdf(-b):cdf(b)-cdf(a);
const value=x=>{x=Math.abs(x);return x<.5?.75-x*x:x<1.5?.5*(1.5-x)**2:0;};
const near=(a,b,e=1e-11)=>assert.ok(Math.abs(a-b)<e,`${a} != ${b}`);
const point=i=>[i&1,(i>>1)&1,i>>2];
const dot=(a,b)=>a.reduce((s,x,i)=>s+x*b[i],0);
function tet(p,phi){
  const [a,b,c]=[0,1,2].sort((a,b)=>p[b]-p[a]);
  return (1-p[a])*phi[0]+(p[a]-p[b])*phi[1<<a]+(p[b]-p[c])*phi[(1<<a)|(1<<b)]+p[c]*phi[7];
}
function solidKernel(phi,offset,width=[1,1,1]){
  const gradient=[0,1,2].map(a=>Math.abs(phi.reduce((s,x,i)=>s+((i>>a)&1?1:-1)*x,0)/width[a]));
  const a=gradient.indexOf(Math.max(...gradient)),b=(a+1)%3,c=(a+2)%3;
  if(phi.every((x,i)=>x===phi[i&(1<<a)])){
    const left=phi[0],right=phi[1<<a];if(left>=0&&right>=0)return 0;
    let lo=0,hi=1;
    if(left>=0||right>=0){const root=left/(left-right);if(left>=0)lo=root;else hi=root;}
    return integral(offset[a]+lo*width[a],offset[a]+hi*width[a])*integral(offset[b],offset[b]+width[b])*integral(offset[c],offset[c]+width[c]);
  }
  const nodes=[.033765242898424,.169395306766868,.380690406958402,.619309593041598,.830604693233132,.966234757101576];
  const weights=[.0856622461895852,.180380786524069,.233956967286346,.233956967286346,.180380786524069,.0856622461895852];
  let result=0;
  for(let i=0;i<6;i++)for(let j=0;j<6;j++){
    const p=[0,0,0];p[b]=nodes[i];p[c]=nodes[j];
    const splits=[0,Math.min(p[b],p[c]),Math.max(p[b],p[c]),1];let line=0;
    for(let k=0;k<3;k++){
      let lo=splits[k],hi=splits[k+1];p[a]=lo;const left=tet(p,phi);p[a]=hi;const right=tet(p,phi);
      if(left>=0&&right>=0)continue;
      if(left>=0||right>=0){const root=lo+(hi-lo)*left/(left-right);if(left>=0)lo=root;else hi=root;}
      line+=integral(offset[a]+lo*width[a],offset[a]+hi*width[a]);
    }
    result+=line*weights[i]*weights[j]*value(offset[b]+nodes[i]*width[b])*value(offset[c]+nodes[j]*width[c])*width[b]*width[c];
  }
  return result;
}
test('quadratic kernel CDF is normalized, symmetric, continuous and differentiates to the transfer kernel',()=>{
  near(cdf(-1.5),0);near(cdf(1.5),1);
  for(let i=-150;i<=150;i++){
    const x=i/100,e=1e-6;
    near(cdf(x)+cdf(-x),1);near((cdf(x+e)-cdf(x-e))/(2*e),value(x),1e-8);
  }
  near(integral(-1.5,-.5),1/6);near(integral(-.5,.5),2/3);near(integral(.5,1.5),1/6);
});
test('fractional room floor has .0703125 missing support, not the voxel-fraction .125',()=>{
  const h=.16,sampleY=.12;
  near(cdf(-sampleY/h),.0703125);
  assert.ok(Math.abs(cdf(-sampleY/h)-.75/6)>.05);
});
test('tetrahedral interpolation reproduces arbitrary planes across axis-order transitions',()=>{
  for(const n of [[.3,.7,1.4],[1,0,0],[-1,2,-3]]){
    const phi=Array.from({length:8},(_,i)=>dot(point(i),n)-.71);
    for(let i=0;i<200;i++){
      const p=[(i*17%199)/199,(i*89%197)/197,(i*53%193)/193];
      near(tet(p,phi),dot(p,n)-.71);
    }
  }
});
test('cached solid integral is exact for translated axis planes, corners and clipped terminal voxels',()=>{
  for(const width of [[1,1,1],[.13,.47,.91]])for(let a=0;a<3;a++)for(const sign of [-1,1])for(let k=0;k<=20;k++){
    const t=k/20,phi=Array.from({length:8},(_,i)=>sign*(point(i)[a]-t));
    for(const offset of [[-.5,-.5,-.5],[-1.5,.5,-.5],[.5,-1.5,.5]]){
      let exact=1;
      for(let axis=0;axis<3;axis++){
        const lo=axis===a&&sign<0?t:0,hi=axis===a&&sign>0?t:1;
        exact*=integral(offset[axis]+lo*width[axis],offset[axis]+hi*width[axis]);
      }
      near(solidKernel(phi,offset,width),exact);
    }
  }
});
test('oblique and curved sampled boundaries keep solid/open kernel measures complementary',()=>{
  const cases=[Array.from({length:8},(_,i)=>dot(point(i),[.3,.7,1.4])-.8),
    Array.from({length:8},(_,i)=>Math.hypot(...point(i).map(x=>x-.13))-.78),
    [1,-1,-.2,1.3,-2,1,.7,-.3]];
  for(const phi of cases)for(const offset of [[-.5,-.5,-.5],[-1.5,.5,-.5]]){
    const s=solidKernel(phi,offset),air=solidKernel(phi.map(x=>-x),offset);
    const full=offset.reduce((p,x)=>p*integral(x,x+1),1);
    assert.ok(s>=0&&air>=0&&s<=full&&air<=full);near(s+air,full);
  }
});
test('local cache follows zero-crossing support rather than irrelevant far-field SDF motion',()=>{
  const bits=x=>new Uint32Array(new Float32Array([x]).buffer)[0];
  const fingerprint=cells=>{
    let a=2166136261,b=0x9e3779b9;
    const add=v=>{a=Math.imul(a^v,16777619)>>>0;b=(b^(v+0x9e3779b9+(b<<6)+(b>>>2)))>>>0;};
    for(const phi of cells){
      const saved=[a,b];a=2166136261;b=0x9e3779b9;
      const type=phi.every(x=>x>=0)?0:phi.every(x=>x<=0)?1:2;
      add(type);if(type===2)for(const x of phi)add(bits(x));
      const voxel=[a,b];[a,b]=saved;add(voxel[0]);add(voxel[1]);
    }
    return [a,b];
  };
  const open=Array(8).fill(2),closed=Array(8).fill(-3),cut=[-.5,.5,-.5,.5,-.5,.5,-.5,.5];
  const original=fingerprint([open,cut,closed]);
  assert.deepEqual(original,fingerprint([open.map(x=>x+7),cut,closed.map(x=>x-9)]));
  assert.notDeepEqual(original,fingerprint([open,cut.map(x=>x+.00001),closed]));
  assert.notDeepEqual(original,fingerprint([open,cut,open]));
  assert.notDeepEqual(original,fingerprint([closed,cut,open]));
});
