import {statSync,readdirSync} from 'node:fs';
import {join} from 'node:path';
import assert from 'node:assert/strict';
import {readCapture} from './validate-captures.mjs';
const folder=process.argv[2];
if(!folder)throw new Error('Usage: node engine/check-cut-cells-parity.mjs <runtime folder>');
const built=Math.max(statSync(join(folder,'NVMatrixFluidLab.exe')).mtimeMs,
  ...readdirSync(join(folder,'shaders')).filter(n=>n.endsWith('.dxil')).map(n=>statSync(join(folder,'shaders',n)).mtimeMs));
const checks=[];
for(const scene of ['calm','room-orbit','wake']){
  const [a,b]=['off','on'].map(mode=>{
    const path=join(folder,`cut-parity-${scene}-${mode}`);assert.ok(statSync(`${path}.json`).mtimeMs>=built,`Stale ${path}`);return readCapture(path);
  });
  assert.equal(a.report.transportHash,b.report.transportHash);
  const channels=a.channels.map((c,i)=>{
    assert.deepEqual(c.header,b.channels[i].header);let different=0,max=0;
    for(let k=0;k<c.values.length;k++){const error=Math.abs(c.values[k]-b.channels[i].values[k]);max=Math.max(max,error);different+=error!==0;}
    assert.equal(different,0,`${scene} channel ${i} changed`);return {channel:i,different,max};
  });
  checks.push({scene,channels});
}
console.log(JSON.stringify({passed:true,scope:'Exact captured lighting, caustic, RR guide and output parity for the opt-in solid capacity observer.',checks},null,2));
