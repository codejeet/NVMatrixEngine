import {readFileSync} from 'node:fs';
import assert from 'node:assert/strict';
import {pathToFileURL} from 'node:url';
function half(h){const s=h&32768?-1:1,e=(h>>10)&31,m=h&1023;return e===31?(m?NaN:s*Infinity):s*(e?2**(e-15)*(1+m/1024):2**-14*m/1024);}
export function photonSourceFlux(report){
 const playable=report.fixture===false,preset=playable?(report.experience?.environment??0):0;
 const flood=preset===3?0:(report.waterFloodWatts??28)*(preset===2?2:1);
 return (preset?0:40)+(report.waterEnabled?flood:0)+
   (!preset&&report.lasersEnabled?(report.laserWattsEach??1.5)*(1+Number(report.waterEnabled)):0)+
   (playable&&report.experience?.flashlight?8:0);
}
export function readCapture(prefix,expectedAge){
 const report=JSON.parse(readFileSync(`${prefix}.json`,'utf8')),b=readFileSync(`${prefix}.inputs`);let offset=12;
 assert.equal(b.subarray(0,8).toString(),'PTGI0001');const count=b.readUInt32LE(8);assert.ok(count===9||count===10);const channels=[];
 for(let i=0;i<count;i++){
   assert.ok(offset+20<=b.length);const header=Array.from({length:5},(_,k)=>b.readUInt32LE(offset+k*4));offset+=20;
   const [index,w,h,format,stride]=header;assert.equal(index,i);assert.ok(w&&h);
   const halfFormat=[10,34,54].includes(format);assert.ok(halfFormat||[2,41].includes(format));
   const size=halfFormat?2:4,bytes=stride*h;assert.ok(offset+bytes<=b.length);
   const values=new Float32Array(bytes/size);
   for(let j=0;j<values.length;j++){const p=offset+j*size;values[j]=halfFormat?half(b.readUInt16LE(p)):b.readFloatLE(p);assert.ok(Number.isFinite(values[j]),`nonfinite channel ${i}, scalar ${j}`);}
   channels.push({header,values});offset+=bytes;
 }
 assert.equal(offset,b.length);
 const atlas=channels[8],aw=atlas.header[1];assert.ok(aw===1024||aw===1280);assert.equal(atlas.header[2],256);
 let power=[0,0,0],nonzero=0,minAge=Infinity,maxAge=0,maxValue=0;
 for(let y=0;y<256;y++)for(let x=0;x<aw;x++){
   const p=(y*aw+x)*4,chart=Math.floor(x/256),area=(chart===4?13.68:chart===0?168:chart===1?72:84)/65536;
   const v=atlas.values,fluid=channels[9]?.values;for(let c=0;c<3;c++){assert.ok(v[p+c]>=0);const value=v[p+c]+(fluid?.[p+c]??0);if(fluid)assert.ok(fluid[p+c]>=0);power[c]+=value*area;maxValue=Math.max(maxValue,value);}
   if(v[p+1]+(fluid?.[p+1]??0)>0)nonzero++;minAge=Math.min(minAge,v[p+3]);maxAge=Math.max(maxAge,v[p+3]);
 }
 const playable=report.fixture===false;
 if(!playable||report.photonCounters[3]>0)assert.ok(power[1]>0&&nonzero>0,'atlas has no caustics');
 else assert.equal(nonzero,0,'blocked receiver retained stale caustics');
 const flux=photonSourceFlux(report);
 assert.ok(power.every(x=>x<=8*flux+1e-6),'atlas creates optical energy');
 if(report.photonEnergyWatts){
   const [emitted,...outcomes]=report.photonEnergyWatts;
   assert.ok(Math.abs(emitted-flux)<.07,'radiant source power/sample normalization mismatch');
   assert.ok(Math.abs(emitted-outcomes.reduce((a,b)=>a+b,0))<.15,'photon energy accounting failed');
 }
 const moving=report.historyResets===report.frames;
 if(expectedAge!==undefined)assert.equal(minAge,expectedAge);
 else if(playable)assert.ok(minAge>=1&&maxAge<=report.historyLength,'playable history age out of bounds');
 else assert.equal(minAge,moving?1:Math.min(report.frames,report.historyLength));
 assert.equal(minAge,maxAge);
 assert.equal(report.photonCounters[5],0);assert.equal(report.photonCounters[6],0);
 const motion=channels[1].values;let motionMax=0;for(const x of motion)motionMax=Math.max(motionMax,Math.abs(x));
 if(moving)assert.ok(motionMax>0,'moving prism lacks motion vectors');
 if(report.orbitTest){
   assert.ok(motionMax>.01,'scripted orbit lacks camera motion vectors');
   assert.equal(report.rrHistoryResets,1,'orbit discarded reconstruction history');
 }
 if(report.rollingTest){
   assert.ok(motionMax>.01,'rolling lacks ball/follow-camera motion vectors');
   assert.equal(report.rrHistoryResets,1,'rolling discarded reconstruction history');
 }
 return {report,channels,summary:{powerXYZ:power,nonzeroTexels:nonzero,maxIrradiance:maxValue,atlasAge:minAge,motionMax}};
}
if(process.argv[1]&&import.meta.url===pathToFileURL(process.argv[1]).href){
const [first,second]=process.argv.slice(2);
if(!first)throw new Error('Usage: node engine/validate-captures.mjs prefix [comparison-prefix]');
const a=readCapture(first);console.log(JSON.stringify({prefix:first,...a.summary},null,2));
if(second){
 const b=readCapture(second);assert.equal(a.report.frames,b.report.frames);assert.equal(a.report.photonsPerFrame,b.report.photonsPerFrame);
 assert.equal(a.report.transportHash,b.report.transportHash,'A/B fixtures have different transport state');
 assert.equal(a.report.prismAngle,b.report.prismAngle,'A/B prism angles differ');
 let error=0,magnitude=0,max=0;
 const x=a.channels[8].values,y=b.channels[8].values;assert.equal(x.length,y.length);
 for(let i=0;i<x.length;i++)if(i%4!==3){const d=Math.abs(x[i]-y[i]);error+=d;magnitude+=Math.abs(x[i]);max=Math.max(max,d);}
 const relativeL1=error/Math.max(magnitude,1e-30);assert.ok(relativeL1<.005,`atlas disagreement ${(relativeL1*100).toFixed(4)}%`);
 console.log(JSON.stringify({comparison:second,atlasRelativeL1:relativeL1,atlasMaxAbs:max,comparisonSummary:b.summary},null,2));
}
console.log('PASS: finite DLSS guides, positive bounded caustics, history age, and optional atlas parity.');
}
