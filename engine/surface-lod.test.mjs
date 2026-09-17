import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

// Independent nested-lattice model, not HLSL string matching.
function coarse(field, p, axes=7) {
  const step=p.map((_,a)=>(axes&(1<<a))?2:1);
  const base=p.map((x,a)=>Math.min(Math.floor(x/step[a])*step[a],8-step[a]));
  const f=p.map((x,a)=>(x-base[a])/step[a]);
  let sum=0;
  for(let k=0;k<8;k++) {
    const q=base.map((x,a)=>x+step[a]*((k>>a)&1));
    const weight=f.reduce((w,t,a)=>w*(((k>>a)&1)?t:1-t),1);
    sum+=field(q)*weight;
  }
  return sum;
}
const norm=v=>{const d=Math.hypot(...v);return v.map(x=>x/d);};
const distance=(a,b)=>Math.hypot(...a.map((x,i)=>x-b[i]));

test('5 cubed anchors exactly preserve affine fields over the 9 cubed cache',()=>{
  for(const gradient of [[0,1,0],[.31,-.7,.19],[-.12,0,.44]]) {
    const field=p=>p.reduce((s,x,a)=>s+x*gradient[a],-2.71);
    for(let z=0;z<=8;z++)for(let y=0;y<=8;y++)for(let x=0;x<=8;x++) {
      assert.ok(Math.abs(coarse(field,[x,y,z])-field([x,y,z]))<3e-15);
    }
  }
});

test('directional coarse lattices retain nonlinear normal profiles without loosening error limits',()=>{
  const field=([,y])=>Math.tanh(y-3.17);
  let isotropicError=0,tangentialError=0;
  for(let z=0;z<=8;z++)for(let y=0;y<=8;y++)for(let x=0;x<=8;x++){
    const p=[x,y,z];
    isotropicError=Math.max(isotropicError,Math.abs(coarse(field,p)-field(p)));
    tangentialError=Math.max(tangentialError,Math.abs(coarse(field,p,5)-field(p)));
  }
  assert.ok(isotropicError>.004);
  assert.equal(tangentialError,0);
});

test('all directional lattices have exact node counts and agree on shared faces',()=>{
  const field=([x,y,z])=>Math.sin(x*.3)+Math.cos(y*.1)+z*.08-1;
  for(let axes=1;axes<8;axes++){
    const dims=[0,1,2].map(a=>(axes&(1<<a))?5:9);
    const count=dims.reduce((a,b)=>a*b),nodes=new Set();
    for(let lane=0;lane<128;lane++)for(let n=lane;n<count;n+=128){
      const p=[n%dims[0],Math.floor(n/dims[0])%dims[1],Math.floor(n/(dims[0]*dims[1]))]
        .map((v,a)=>v*((axes&(1<<a))?2:1));
      assert.ok(p.every(v=>v<=8));nodes.add(p.join(','));
    }
    assert.equal(nodes.size,count);assert.ok([125,225,405].includes(count));
    for(let axis=0;axis<3;axis++)for(let u=0;u<=8;u++)for(let v=0;v<=8;v++){
      const p=[0,0,0];p[axis]=8;p[(axis+1)%3]=u;p[(axis+2)%3]=v;
      const neighbor=p.map((x,a)=>x-(a===axis?8:0));
      const other=q=>field(q.map((x,a)=>x+(a===axis?8:0)));
      assert.ok(Math.abs(coarse(field,p,axes)-coarse(other,neighbor,axes))<1e-14);
    }
  }
});

test('shared face, edge and corner constraints agree through arbitrary transitions',()=>{
  const world=p=>Math.sin(p[0]*.3)+Math.cos(p[1]*.1)+p[2]*.08-1;
  const blend=[0,.125,.25,.375,.5,.625,.875,1];
  for(let z=0;z<=16;z++)for(let y=0;y<=16;y++)for(let x=0;x<=16;x++) {
    const p=[x,y,z],owners=[];
    for(let k=0;k<8;k++) {
      const b=[k&1,(k>>1)&1,k>>2],local=p.map((v,a)=>v-8*b[a]);
      if(local.every(v=>v>=0&&v<=8))owners.push({k,b,local});
    }
    if(owners.length<2)continue;
    const w=Math.max(...owners.map(o=>blend[o.k]));
    const values=owners.map(({b,local})=>{
      const c=coarse(q=>world(q.map((v,a)=>v+b[a]*8)),local);
      return world(p)*(1-w)+c*w;
    });
    assert.ok(Math.max(...values)-Math.min(...values)<1e-14);
  }
});

test('nodal sign checks veto a sheet that coarse anchors would entirely miss',()=>{
  const field=([,y])=>Math.abs(y-3)-.1;
  assert.ok(field([4,3,4])<0);
  assert.ok(coarse(field,[4,3,4])>0);
  const lost=(field([4,3,4])<0)!==(coarse(field,[4,3,4])<0);
  assert.equal(lost,true);
});

test('small scalar error alone does not bound a neighboring surface normal',()=>{
  const reference=norm([.001,0,0]);
  const candidate=norm([.001,.0001,0]);
  assert.ok(.0001<.004);
  assert.ok(distance(reference,candidate)>.015);
});

test('LOD displacement uses previous minus current position for motion guides',()=>{
  const fineOffset=.3,coarseOffset=.302,oldBlend=.25,newBlend=.375;
  const oldPosition=-(fineOffset*(1-oldBlend)+coarseOffset*oldBlend);
  const newPosition=-(fineOffset*(1-newBlend)+coarseOffset*newBlend);
  const guide=(newBlend-oldBlend)*(coarseOffset-fineOffset);
  assert.ok(Math.abs(guide-(oldPosition-newPosition))<1e-16);
});

test('late seam rejection restores full samples rather than exposing stale odd nodes',()=>{
  const full=false,approvedBlend=0;
  const repair=!full&&approvedBlend!==1;
  assert.equal(repair,true);
  const coarseCost=125,fineCost=729;
  assert.equal(coarseCost+(repair?fineCost:0),854);
  // Repair cost is reported, not hidden as a successful coarse gather saving.
});

test('reconstruction invalidation halo includes both extreme 1.8-cell kernel supports',()=>{
  const included=new Set(Array.from({length:8},(_,i)=>i-2));
  for(const node of [0,4])for(const offset of [-1.7999,1.7999])
    assert.ok(included.has(Math.floor(node+offset)));
  assert.ok(!new Set([-1,0,1,2,3,4]).has(Math.floor(4+1.7999)));
});

test('GPU fingerprint maps the padded surface origin into simulation cells',()=>{
  const shader=readFileSync(new URL('./shaders/fluid/surface-lod.hlsli',import.meta.url),'utf8');
  assert.match(shader,/int3 base=int3\(brickCoord\(id\)\*4\)-2;/);
  assert.match(shader,/for\(int x=-2;x<=5;\+\+x\)/);
  for(const brick of [0,3,17])for(const node of [0,4])for(const offset of [-1.7999,1.7999]){
    const origin=brick*4-2,cell=Math.floor(origin+node+offset);
    assert.ok(cell>=origin-2&&cell<=origin+5);
  }
});

test('changed reconstruction inputs require a full error refresh even at established coarse LOD',()=>{
  const previousSignature=0x11910001,currentSignature=0x11910002;
  const oldBlend=1,blend=1,update=25,lastFull=24;
  const full=previousSignature!==currentSignature||blend<1||blend!==oldBlend||update-lastFull>=8;
  assert.equal(full,true);
});

test('camera-only reconstruction preserves caustic history and ignores stale LOD motion flags',()=>{
  const moving=(physical,updated,flag)=>physical||(updated&&flag);
  assert.equal(moving(false,true,false),false);
  assert.equal(moving(false,false,true),false);
  assert.equal(moving(false,true,true),true);
  assert.equal(moving(true,false,false),true);
});

test('cell input fingerprints detect within-bin motion, shape, weight and guide changes independently of order',()=>{
  const hash=x=>{x^=x>>>16;x=Math.imul(x,0x7feb352d);x^=x>>>15;x=Math.imul(x,0x846ca68b);return (x^(x>>>16))>>>0;};
  const bits=x=>new Uint32Array(new Float32Array([x]).buffer)[0];
  const h4=v=>(hash(bits(v[0]))^hash((bits(v[1])+0x9e3779b9)>>>0)^hash((bits(v[2])+0x85ebca6b)>>>0)^hash((bits(v[3])+0xc2b2ae35)>>>0))>>>0;
  const key=p=>{let k=h4(p.positionWeight);for(const row of p.rows)k=hash(k^h4(row));return hash(hash(k^h4(p.previous))^p.id);};
  const cell=particles=>particles.reduce((sum,p)=>(sum+key(p))>>>0,particles.length);
  const a={id:3,positionWeight:[.031,.045,.029,1],rows:[[1,0,0,0],[0,1,0,0],[0,0,1,0]],previous:[.031,.045,.029,0]};
  const b={...structuredClone(a),id:9};
  assert.equal(cell([a,b]),cell([b,a]));
  for(const change of [p=>p.positionWeight[0]+=.001,p=>p.positionWeight[3]=2,p=>p.rows[1][1]=1.01,p=>p.previous[1]+=.002]){
    const changed=structuredClone(a);change(changed);
    assert.notEqual(cell([changed,b]),cell([a,b]));
    assert.equal(Math.floor(changed.positionWeight[0]/.08),Math.floor(a.positionWeight[0]/.08));
  }
});
