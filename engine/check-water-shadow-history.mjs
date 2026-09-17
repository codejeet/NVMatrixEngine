import assert from 'node:assert/strict';
import {join} from 'node:path';
import {readCapture} from './validate-captures.mjs';

const [folder,prefix,reference]=process.argv.slice(2);
if(!folder||!prefix)throw new Error('Usage: node engine/check-water-shadow-history.mjs runtime-directory prefix [before-prefix]');
function sequence(name,check){
  const phases={};let pausedFoam;
  for(const [phase,end] of [['static',128],['risingOrbit',176],['topDownOrbit',224],['liveWaterOrbit',272],['afterRolling',320]]){
    let previous,delta=0,magnitude=0,mean=0,zeros=0,minAge=Infinity,maxAge=0;
    let resetCount=0;
    for(let frame=end-3;frame<=end;++frame){
      const c=readCapture(join(folder,`${name}-${frame}`)),r=c.report,atlas=c.channels[9];
      assert.equal(r.frames,frame);assert.equal(r.dlssEvaluations,frame);
      assert.equal(r.rrHistoryResets,1,'Orbit discarded global reconstruction history');
      assert.ok(atlas,'Missing dynamic water atlas');
      resetCount=r.fluidHistoryResets;
      if(check)assert.equal(resetCount,1,'Ordinary body/camera motion restarted the water atlas');
      if(check&&['advected-cellular-layer','advected-grain-layer'].includes(r.whitewater?.foamRendering)){
        const w=r.whitewater;
        assert.equal(w.invalid,0);assert.ok(w.foamMaxDensity>=0&&w.foamMaxDensity<=4);
        if(end<=224){
          const state=[w.foamActiveNodes,w.foamMaxDensity,w.foam,w.bubbles,w.spray];
          if(pausedFoam)assert.deepEqual(state,pausedFoam,'Paused foam layer drifted during camera motion');
          else pausedFoam=state;
        }
      }
      const width=atlas.header[1],v=atlas.values;
      // Chart zero is the fixed world-space floor. No camera reprojection or
      // denoiser is involved in this measurement. Live phases include real motion.
      for(let y=0;y<256;++y)for(let x=0;x<256;++x){
        const i=(y*width+x)*4,value=v[i+1],age=v[i+3];
        minAge=Math.min(minAge,age);maxAge=Math.max(maxAge,age);
        mean+=value;zeros+=Number(value<1e-8);
        if(previous){delta+=Math.abs(value-previous[i+1]);magnitude+=.5*(value+previous[i+1]);}
      }
      previous=v;
    }
    if(check)assert.ok(minAge>=4&&maxAge<=32,'Animated water did not retain its bounded history');
    phases[phase]={resets:resetCount,age:[minAge,maxAge],meanIrradianceY:mean/(4*65536),
      zeroFraction:zeros/(4*65536),floorRelativeDelta:delta/Math.max(magnitude,1e-10)};
  }
  return phases;
}
const current=sequence(prefix,true),before=reference?sequence(reference,false):undefined;
console.log(JSON.stringify({prefix,current,reference,before,
  note:'Consecutive real-frame world-space water irradiance, not a perceptual image score. Live fluid/rigid-body trajectories may differ between runs; do not interpret their brightness/motion differences as estimator bias or pure noise.'},null,2));
console.log('PASS: finite guides, photon energy accounting, preserved water history and uninterrupted RR.');
