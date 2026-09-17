import assert from 'node:assert/strict';
import {join} from 'node:path';
import {readCapture} from './validate-captures.mjs';
const folder=process.argv[2];
if(!folder)throw new Error('Usage: node engine/validate-water.mjs capture-directory');
const results=new Map();
for(const name of ['green','blue','red','flat','no-laser','off','fixed','no-haze']) {
  const c=readCapture(join(folder,`water-${name}`)),r=c.report,v=c.channels[8].values,w=c.channels[8].header[1];
  assert.equal(w,1280);assert.equal(r.rrHistoryResets,1);assert.equal(r.frames,192);
  const samples=[];let poolY=0;
  // Interior, 4x4-texel averaged irradiance: compare focusing, not photon grain,
  // aperture edges, checker albedo, tone mapping, or RR's reconstruction.
  for(let y=72;y<184;y+=4)for(let x=72;x<184;x+=4){
    let value=0;for(let j=0;j<4;j++)for(let i=0;i<4;i++)value+=v[((y+j)*w+x+i+1024)*4+1];
    samples.push(value/16);
  }
  for(let y=0;y<256;y++)for(let x=1024;x<1280;x++)poolY+=v[(y*w+x)*4+1]*13.68/65536;
  const mean=samples.reduce((a,b)=>a+b)/samples.length;
  const cv=mean?Math.sqrt(samples.reduce((a,x)=>a+(x-mean)**2,0)/samples.length)/mean:0;
  const [emitted,...outcomes]=r.photonEnergyWatts;
  const summary={name,waterFloorWatts:r.waterFloorWatts,waterEntries:r.waterPhotonEntries,beamSegments:r.beamSegments,
    beamTruncated:r.beamTruncated,photonTruncated:r.photonCounters[4],cameraTruncated:r.cameraTruncatedLastFrame,
    ledgerErrorWatts:Math.abs(emitted-outcomes.reduce((a,b)=>a+b,0)),poolPhotometricY:poolY,
    interiorMeanIrradiance:mean,interiorCoefficientOfVariation:cv,medianMs:r.medianMs,...c.summary};
  results.set(name,{summary,atlas:(name==='green'||name==='fixed')?v:null});
}
const get=name=>results.get(name).summary;
assert.ok(get('no-laser').interiorCoefficientOfVariation>5*get('flat').interiorCoefficientOfVariation,'Water normals do not focus light');
assert.ok(Math.abs(get('no-laser').waterFloorWatts/get('flat').waterFloorWatts-1)<.02,'Ripples created/lost excessive radiant flux');
assert.ok(get('blue').waterFloorWatts>get('green').waterFloorWatts&&get('green').waterFloorWatts>get('red').waterFloorWatts,'Water absorption is not spectral');
assert.equal(get('off').waterFloorWatts,0);assert.equal(get('off').poolPhotometricY,0);
assert.equal(get('no-laser').beamSegments,0);assert.equal(get('flat').beamSegments,0);
assert.ok(get('no-haze').waterFloorWatts>get('green').waterFloorWatts,'Air extinction does not remove power');
let error=0,magnitude=0;const a=results.get('green').atlas,b=results.get('fixed').atlas;
for(let i=0;i<a.length;i++)if(i%4!==3){error+=Math.abs(a[i]-b[i]);magnitude+=Math.abs(a[i]);}
const fixedFloatRelativeL1=error/magnitude;assert.ok(fixedFloatRelativeL1<.005,'Fixed-point and float accumulation disagree');
console.log(JSON.stringify({fixedFloatRelativeL1,runs:[...results.values()].map(x=>x.summary)},null,2));
console.log('PASS: finite guides, spectral absorption, photon energy, real focusing, disabled controls and fixed/float parity.');
