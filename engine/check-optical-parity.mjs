import {statSync,readdirSync} from 'node:fs';
import {join} from 'node:path';
import assert from 'node:assert/strict';
import {readCapture} from './validate-captures.mjs';
const folder=process.argv[2];
if(!folder)throw new Error('Usage: node engine/check-optical-parity.mjs <runtime folder>');
const built=Math.max(statSync(join(folder,'NVMatrixFluidLab.exe')).mtimeMs,
  ...readdirSync(join(folder,'shaders')).filter(n=>n.endsWith('.dxil')).map(n=>statSync(join(folder,'shaders',n)).mtimeMs));
const checks=[];
for(const scene of ['room','prism','room-orbit']){
  const captures=['cached','retraced','observer'].map(mode=>{
    const path=join(folder,`optical-parity-${scene}-${mode}`);
    assert.ok(statSync(`${path}.json`).mtimeMs>=built,`Stale ${path}`);
    return readCapture(path);
  });
  for(let j=1;j<3;j++){
    const a=captures[0],b=captures[j];assert.equal(a.report.transportHash,b.report.transportHash);
    const channels=a.channels.map((c,i)=>{
      assert.deepEqual(c.header,b.channels[i].header);
      let max=0,different=0;
      for(let k=0;k<c.values.length;k++){
        const error=Math.abs(c.values[k]-b.channels[i].values[k]);max=Math.max(max,error);different+=error!==0;
      }
      assert.equal(different,0,`${scene} ${j===1?'primary-cache':'observer'} channel ${i} changed`);
      return {channel:i,different,max};
    });
    checks.push({scene,comparison:j===1?'primary-cache':'observer',channels});
  }
}
console.log(JSON.stringify({passed:true,scope:'Bit-identical raw lighting, RR guides, RR output and photon atlases for cached/retraced primary and passive optical observer.',checks},null,2));
