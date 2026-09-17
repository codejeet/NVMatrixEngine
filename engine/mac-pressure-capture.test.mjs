import test from 'node:test';
import assert from 'node:assert/strict';
import {execFileSync} from 'node:child_process';
import {join} from 'node:path';

test('pressure failure decoder preserves legacy formats and the cut-cell precision sidecar',()=>{
  for(const [version,cut] of [[1,false],[2,false],[3,false],[3,true]]){
    const rowBytes=version>=3?216:208,pressureBytes=version>=2?8:4;
    const row=336,map=row+rowBytes,cells=map+4,pressure=cells+16;
    const exact=pressure+pressureBytes+32+16384,scalar=exact+(cut?64:0);
    const data=Buffer.alloc(scalar+80);
    const u=(at,v)=>data.writeUInt32LE(v,at),f=(at,v)=>data.writeFloatLE(v,at);
    u(0,0x3150474d);u(4,version);u(8,1);u(16,304);u(20,1);u(24,1);u(28,cut?1:0);
    for(let i=0;i<4;i++)u(32+4*i,1);
    f(328,.5);f(row+4,2);f(row+8,3);f(cells+8,1);
    if(version>=3){f(row+208,2);f(row+212,.125);}
    if(pressureBytes===8)data.writeDoubleLE(1.5,pressure);else f(pressure,1.5);
    if(cut){data.writeDoubleLE(3,exact);data.writeDoubleLE(2,exact+8);data.writeDoubleLE(2,exact+16);}
    const r=JSON.parse(execFileSync(process.execPath,[join(import.meta.dirname,'inspect-mac-pressure.mjs'),'-'],{input:data,encoding:'utf8'}));
    assert.equal(r.version,version);assert.equal(r.cutPressure,cut);assert.equal(r.active,1);
    assert.equal(r.worst[0].divergence,0);assert.equal(r.worst[0].p,1.5);
    assert.equal(r.worst[0].volumeUnits,version>=3?.125:1);
  }
});
