// Read-only, same-build isolation audit: changing passive bulk transport must
// leave authoritative particles and every captured optical guide unchanged.
import {readdirSync,statSync} from 'node:fs';
import {join} from 'node:path';
import assert from 'node:assert/strict';
import {readCapture} from './validate-captures.mjs';
const folder=process.argv[2], boundedMode=process.argv.includes('--bounded'), capacityMode=process.argv.includes('--capacity'), scenes=process.argv.slice(3).filter(v=>!['--capacity','--bounded'].includes(v));
if(!folder)throw new Error('Usage: node engine/check-bulk-projected.mjs RUNTIME [scenes...]');
const built=Math.max(statSync(join(folder,'NVMatrixFluidLab.exe')).mtimeMs,
  ...readdirSync(join(folder,'shaders')).filter(n=>n.endsWith('.dxil')).map(n=>statSync(join(folder,'shaders',n)).mtimeMs));
const results=[];
for(const scene of scenes.length?scenes:['calm','room','wake','adaptive']){
  const runs=(boundedMode?['bounded','capacity']:capacityMode?['capacity','projected']:['projected','legacy']).map(mode=>{
    const prefix=join(folder,`bulk-${mode}-${scene}`);
    assert.ok(statSync(prefix+'.json').mtimeMs>=built,`Stale ${prefix}`);
    return readCapture(prefix);
  });
  const [a,b]=runs.map(r=>r.report);
  assert.equal(a.floatAtomics,false);assert.equal(b.floatAtomics,false);
  assert.ok(a.fluid.bulk.projectedFlux);
  if(boundedMode)assert.ok(a.fluid.bulk.phaseLimiter&&!b.fluid.bulk.phaseLimiter&&a.fluid.bulk.sourceAllocation.validated&&b.fluid.bulk.sourceAllocation.validated);
  else if(capacityMode)assert.ok(a.fluid.bulk.sourceAllocation.validated&&!b.fluid.bulk.sourceAllocation);
  else assert.ok(!b.fluid.bulk.projectedFlux);
  assert.ok(a.fluid.bulk.validated&&b.fluid.bulk.validated&&a.fluid.validated&&b.fluid.validated);
  for(const key of ['frames','transportHash','photonCounters','photonEnergyWatts'])assert.deepEqual(a[key],b[key],`${scene}: ${key}`);
  for(const key of ['steps','restMassUnits','postStepMaxRelativeDensity','postStepOccupiedVolume','meanHeight','maxParticleCfl'])
    assert.deepEqual(a.fluid[key],b.fluid[key],`${scene}: ${key}`);
  assert.equal(a.fluidSurface.tetrahedralVolumeEstimate,b.fluidSurface.tetrahedralVolumeEstimate);
  const channels=runs[0].channels.map((channel,i)=>{
    const other=runs[1].channels[i];assert.deepEqual(channel.header,other.header);
    const [,w,h,format,stride]=channel.header,size=[10,34,54].includes(format)?2:4;
    const components={2:4,10:4,34:2,41:1,54:1}[format];
    for(let y=0;y<h;y++)for(let x=0;x<w*components;x++){
      const index=y*stride/size+x;
      assert.equal(channel.values[index],other.values[index],`${scene}: channel ${i}, ${index}`);
    }
    return {channel:i,different:0};
  });
  results.push({scene,channels});
}
console.log(JSON.stringify({passed:true,scope:'Exact authoritative diagnostics and optical-channel isolation, not correctness of passive liquid support.',results},null,2));
