// Read-only comparison of separately run, deterministic cached/full workloads.
import {readdirSync,statSync} from 'node:fs';
import {join} from 'node:path';
import assert from 'node:assert/strict';
import {readCapture} from './validate-captures.mjs';
const folder=process.argv[2];
if(!folder)throw new Error('Usage: node engine/check-density-kernel.mjs RUNTIME');
const built=Math.max(statSync(join(folder,'NVMatrixFluidLab.exe')).mtimeMs,
  ...readdirSync(join(folder,'shaders')).filter(n=>n.endsWith('.dxil')).map(n=>statSync(join(folder,'shaders',n)).mtimeMs));
const results=[];
for(const scene of ['calm','room','wake','adaptive']){
  const runs=['cached','full'].map(mode=>{
    const prefix=join(folder,`density-kernel-parity-${mode}-${scene}`);
    assert.ok(statSync(prefix+'.json').mtimeMs>=built,`Stale ${prefix}`);
    return readCapture(prefix);
  });
  const [a,b]=runs.map(r=>r.report),ka=a.fluid.cutCells,kb=b.fluid.cutCells;
  assert.equal(a.floatAtomics,false);assert.equal(b.floatAtomics,false);
  assert.ok(ka.solidKernelEnabled&&kb.solidKernelEnabled&&ka.solidKernelCache&&!kb.solidKernelCache);
  assert.ok(a.fluid.validated&&b.fluid.validated);
  for(const key of ['frames','transportHash','photonCounters','photonEnergyWatts'])assert.deepEqual(a[key],b[key],key);
  for(const key of ['steps','restMassUnits','postStepMaxRelativeDensity','postStepOccupiedVolume','meanHeight','maxParticleCfl'])
    assert.deepEqual(a.fluid[key],b.fluid[key],`${scene}: ${key}`);
  assert.equal(a.fluidSurface.tetrahedralVolumeEstimate,b.fluidSurface.tetrahedralVolumeEstimate);
  // Ignore row padding in a GPU texture readback: it is not an image channel.
  const channels=runs[0].channels.map((channel,i)=>{
    const other=runs[1].channels[i];assert.deepEqual(channel.header,other.header);
    const [,w,h,format,stride]=channel.header,size=[10,34,54].includes(format)?2:4;
    const components={2:4,10:4,34:2,41:1,54:1}[format];
    for(let y=0;y<h;y++)for(let x=0;x<w*components;x++){
      const index=y*stride/size+x;assert.equal(channel.values[index],other.values[index],`${scene}: channel ${i}, ${index}`);
    }
    return {channel:i,different:0};
  });
  assert.ok(ka.lastEndpointKernelReused>0,'No actual cached work');
  if(ka.frameUpdates)assert.ok(ka.lastEndpointKernelRebuilt<kb.lastEndpointKernelRebuilt);
  results.push({scene,density:a.fluid.postStepMaxRelativeDensity,
    rebuilt:ka.lastEndpointKernelRebuilt,reused:ka.lastEndpointKernelReused,channels});
}
console.log(JSON.stringify({passed:true,scope:'Exact same-build captured physics/lighting/guide parity for locally cached versus full solid-kernel integration.',results},null,2));
