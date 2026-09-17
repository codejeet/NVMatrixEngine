import assert from 'node:assert/strict';
import {join} from 'node:path';
import {readCapture} from './validate-captures.mjs';
const [folder,prefix='water-temporal',reference,mode]=process.argv.slice(2);
if(!folder)throw new Error('Usage: node engine/validate-water-temporal.mjs runtime-directory prefix [reference-prefix] [--preserve]');
if(mode!==undefined&&(mode!=='--preserve'||!reference))throw new Error('Only --preserve with a reference sequence is supported');
const luma=(v,i)=>Math.max(0,.2126*v[i]+.7152*v[i+1]+.0722*v[i+2]);
const compress=x=>x/(1+x);
function halton(i,base){let r=0,f=1;while(i){f/=base;r+=f*(i%base);i=Math.floor(i/base);}return r;}
function sample(c,x,y,component=0,components=4,lum=false){
  const [,w,h]=c.header;
  const ix=Math.floor(x),iy=Math.floor(y),fx=x-ix,fy=y-iy;
  const at=(xx,yy)=>{const p=(Math.min(h-1,Math.max(0,yy))*w+Math.min(w-1,Math.max(0,xx)))*components;return lum?luma(c.values,p):c.values[p+component];};
  return (at(ix,iy)*(1-fx)+at(ix+1,iy)*fx)*(1-fy)+(at(ix,iy+1)*(1-fx)+at(ix+1,iy+1)*fx)*fy;
}
function measure(a,b){
  const ac=a.channels,bc=b.channels,[,w,h]=bc[0].header;
  const f=b.report.frames-1,jx=halton(f%1024+1,2)-halton((f-1)%1024+1,2),jy=halton(f%1024+1,3)-halton((f-1)%1024+1,3);
  let raw=0,rr=0,count=0,mean=0;
  for(let y=3;y<h-3;y++)for(let x=3;x<w-3;x++){
    const p=y*w+x,n=bc[3].values,al=bc[4].values,spec=bc[5].values,d=bc[2].values[p];
    // Near-horizontal, transmission-dominant first hits: excludes the opaque
    // floor, room walls, most ball/prism silhouettes and whitewater particles.
    if(n[p*4+1]<.98||n[p*4+3]>.02||al[p*4]+al[p*4+1]+al[p*4+2]>.001||spec[p*4]>.04||d>=199)continue;
    const mx=bc[1].values[p*2],my=bc[1].values[p*2+1],px=x+mx,py=y+my;
    if(px<3||px>w-4||py<3||py>h-4)continue;
    if(sample(ac[3],px+jx,py+jy,1)<.98||sample(ac[4],px+jx,py+jy)>.001)continue;
    if([p-1,p+1,p-w,p+w].some(q=>Math.abs(bc[2].values[q]-d)>.06))continue;
    const now=luma(bc[0].values,p*4),old=sample(ac[0],px+jx,py+jy,0,4,true);
    raw+=Math.abs(compress(now)-compress(old));mean+=compress(now);
    const output=(c,xx,yy)=>sample(c,(xx+.5)*c.header[1]/w-.5,(yy+.5)*c.header[2]/h-.5,0,4,true);
    rr+=Math.abs(compress(output(bc[7],x,y))-compress(output(ac[7],px,py)));
    count++;
  }
  let atlasDelta=0,atlasMagnitude=0;
  for(let i=0;i<bc[9].values.length;i++)if(i%4!==3){atlasDelta+=Math.abs(bc[9].values[i]-ac[9].values[i]);atlasMagnitude+=ac[9].values[i];}
  assert.ok(count>500,'Insufficient calm-water receiver samples');
  return {samples:count,rawDelta:raw/count,rrDelta:rr/count,meanLuminance:mean/count,atlasRelativeDelta:atlasDelta/atlasMagnitude};
}
function sequence(name){
  const phases={};
  for(const [phase,end] of [['static',128],['risingOrbit',176],['topDownOrbit',224],['liveWaterOrbit',272],['afterRolling',320]]){
    const captures=[end-3,end-2,end-1,end].map(f=>{
      const c=readCapture(join(folder,`${name}-${f}`));
      assert.equal(c.report.frames,f);assert.equal(c.report.dlssEvaluations,f);assert.equal(c.report.rrHistoryResets,1);
      return c;
    });
    const pairs=captures.slice(1).map((c,i)=>measure(captures[i],c));
    phases[phase]=Object.fromEntries(Object.keys(pairs[0]).map(k=>[k,pairs.reduce((s,v)=>s+v[k],0)/pairs.length]));
    phases[phase].atlasResets=captures[3].report.historyResets;
    phases[phase].fluidSteps=captures[3].report.fluid.steps;
    phases[phase].fluidHistoryResets=captures[3].report.fluidHistoryResets;
  }
  assert.equal(phases.topDownOrbit.fluidSteps,phases.static.fluidSteps,'Camera-only phase advanced liquid');
  assert.equal(phases.topDownOrbit.atlasResets,phases.static.atlasResets,'Orbit invalidated photon history');
  assert.ok(phases.liveWaterOrbit.fluidSteps>phases.topDownOrbit.fluidSteps,'Live phase froze the simulation');
  if(phases.static.fluidHistoryResets!==undefined)
    assert.equal(phases.afterRolling.fluidHistoryResets,phases.static.fluidHistoryResets,'Rolling discarded water history');
  return phases;
}
const current=sequence(prefix),before=reference?sequence(reference):undefined;
const changes=before?Object.fromEntries(Object.keys(current).map(k=>[k,{
  rawDeltaReductionPercent:100*(1-current[k].rawDelta/before[k].rawDelta),
  rrDeltaReductionPercent:100*(1-current[k].rrDelta/before[k].rrDelta),
  meanChangePercent:100*(current[k].meanLuminance/before[k].meanLuminance-1)}])):undefined;
// Keep measurements available on a failed gate; do not weaken the assertions
// or turn a changed physical trajectory into a passing preservation claim.
console.log(JSON.stringify({note:'Motion-compensated consecutive real frames, raw and RR separately. Reprojection uses the current primary-surface guide, so this metric also includes refracted parallax mismatch; it is not pure estimator variance. Live phases include genuine surface/light changes. FG is off.',comparisonMode:mode==='--preserve'?'preserve':'improve',prefix,current,reference,before,changes},null,2));
if(before&&mode==='--preserve'){
  // Performance changes must retain the already-corrected temporal behavior,
  // not satisfy the older test's demand for another 20-75% flicker reduction.
  for(const phase of Object.keys(current)){
    assert.ok(Math.abs(changes[phase].meanChangePercent)<1,`${phase}: brightness changed`);
    assert.ok(current[phase].rawDelta<=before[phase].rawDelta*1.05,`${phase}: raw stability regressed`);
    assert.ok(current[phase].rrDelta<=before[phase].rrDelta*1.05,`${phase}: RR stability regressed`);
  }
}else if(before){
  for(const phase of ['risingOrbit','topDownOrbit','liveWaterOrbit','afterRolling'])
    assert.ok(Math.abs(changes[phase].meanChangePercent)<3,`${phase}: improvement changed overall brightness`);
  assert.ok(current.topDownOrbit.rawDelta<before.topDownOrbit.rawDelta*.75,'Raw grid minification still flickers');
  assert.ok(current.topDownOrbit.rrDelta<before.topDownOrbit.rrDelta*.8,'Top-down RR output did not improve');
  assert.ok(current.liveWaterOrbit.rrDelta<before.liveWaterOrbit.rrDelta*.85,'Live water orbit did not improve');
  assert.ok(current.afterRolling.rrDelta<before.afterRolling.rrDelta*.25,'Rolling still flashes the water lighting');
}
