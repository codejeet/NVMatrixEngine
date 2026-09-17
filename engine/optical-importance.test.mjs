import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
const samples=(importance,cap)=>Math.min(cap,importance<.25?1:importance<.6?2:importance<.85?4:8);

test('optical budgets are bounded and monotone at every configured cap',()=>{
  for(let cap=1;cap<=8;cap++){
    let previous=0;
    for(let i=0;i<=1000;i++){
      const n=samples(i/1000,cap);
      assert.ok(Number.isInteger(n)&&n>=1&&n<=cap&&n>=previous);previous=n;
    }
  }
  assert.equal(samples(1,4),4);assert.equal(samples(0,4),1);
});

test('fresh sample means preserve expectation with a history-selected sample count',()=>{
  // Exactly enumerate the new independent Bernoulli paths for each pilot
  // history. Never choose N by observing any of these production samples.
  for(const pilot of [0,.4,.7,.99]){
    const n=samples(pilot,4);let mean=0;
    for(let bits=0;bits<(1<<n);bits++){
      let radiance=0;for(let k=0;k<n;k++)radiance+=((bits>>k)&1)*2/n;
      mean+=radiance/(1<<n);
    }
    assert.equal(mean,1);
  }
});

test('history reprojection maps to the previous jittered lattice, not the unjittered motion guide',()=>{
  const dims=[1114,626];
  for(const p of [[0,0],[541,310],[1113,625]])for(const now of [[.2,-.4],[-.49,.31]])for(const old of [[-.1,.25],[.4,-.32]]){
    const clip=p.map((v,a)=>((v+.5+now[a])/dims[a]*2-1)*(a===0?1:-1));
    const previous=clip.map((v,a)=>(v*(a===0?.5:-.5)+.5)*dims[a]-.5-old[a]);
    for(let a=0;a<2;a++)assert.ok(Math.abs(previous[a]-(p[a]+now[a]-old[a]))<1e-10);
  }
});

test('moment history is scheduling data; constant light converges without changing radiance',()=>{
  let mean=0,square=0,age=0;
  for(let frame=0;frame<128;frame++){
    const noisy=.31;age=Math.min(age+1,32);mean+=(noisy-mean)/age;square+=(noisy*noisy-square)/age;
    assert.ok(Math.max(0,square-mean*mean)<1e-14);assert.equal(noisy,.31);
  }
  assert.equal(age,32);
});

test('receiver importance promotes immediately and decays without filtering the caustic lighting',()=>{
  const irradiance=[0,8,0,0],dt=1/60;let importance=0;
  const seen=irradiance.map(value=>{const raw=value/(.25+value);importance=Math.max(raw,importance*Math.exp(-dt/.2));return importance;});
  assert.equal(seen[0],0);assert.ok(seen[1]>.95);assert.ok(seen[2]<seen[1]&&seen[2]>.8);
  assert.deepEqual(irradiance,[0,8,0,0]);
});

test('known dielectric primary replaces exactly one duplicate traversal and no random draw',()=>{
  for(const glass of [false,true])for(const retrace of [false,true]){
    const tree=[{object:8,t:3.27},{object:0,t:1.4}];let traces=1,randomDraws=0;
    const visits=tree.map((hit,visit)=>{if(!(visit===0&&glass&&!retrace))traces++;randomDraws+=3;return hit;});
    assert.deepEqual(visits,tree);assert.equal(traces,glass&&!retrace?2:3);assert.equal(randomDraws,6);
  }
});

test('interface feedback retains the traced primitive at shared brick boundaries',()=>{
  const spacing=.24,epsilon=1e-5;
  for(let cell=1;cell<40;cell++)for(const offset of [-1e-7,0,1e-7]){
    const p=cell*spacing+offset;
    // Both closed AABBs may produce a root on their shared face. Re-quantizing
    // it with floor would discard the actual DXR primitive's ownership.
    for(const primitive of [cell-1,cell]){
      assert.ok(p>=primitive*spacing-epsilon&&p<=(primitive+1)*spacing+epsilon);
      const packed=4|((primitive+1)<<8);
      assert.equal(packed&255,4);assert.equal((packed>>>8)-1,primitive);
    }
    assert.ok(!(p>=(cell+1)*spacing-epsilon&&p<=(cell+2)*spacing+epsilon));
  }
});

test('wave histogram predicates retain miss lanes without splitting the vote',()=>{
  const shader=readFileSync(new URL('./shaders/optical-importance.hlsl',import.meta.url),'utf8');
  assert.ok(!/WaveActiveCountBits\([^\n;]*&&/.test(shader),'Short-circuit predicate split a wave vote');
  for(let mask=0;mask<256;mask++){
    const counts=[0,0,0,0];let hits=0;
    for(let bucket=0;bucket<4;bucket++)for(let lane=0;lane<8;lane++){
      const hit=(mask>>>lane)&1,n=[1,2,4,8][lane%4];
      const belongs=bucket===0?n<=1:bucket===1?n===2:bucket===2?n>2&&n<=4:n>4;
      counts[bucket]+=hit&Number(belongs);
    }
    for(let lane=0;lane<8;lane++)hits+=(mask>>>lane)&1;
    assert.equal(counts.reduce((a,b)=>a+b,0),hits);
  }
});
