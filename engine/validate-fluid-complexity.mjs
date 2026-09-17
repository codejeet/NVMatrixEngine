import assert from 'node:assert/strict';
import {join} from 'node:path';
import {readCapture} from './validate-captures.mjs';
const [folder,reference='adaptive-baseline',candidate='adaptive-observe',repeat]=process.argv.slice(2);
if(!folder)throw new Error('Usage: node engine/validate-fluid-complexity.mjs runtime [reference] [candidate] [baseline-repeat]');
const a=readCapture(join(folder,reference)),b=readCapture(join(folder,candidate));
const repeated=repeat?readCapture(join(folder,repeat)):undefined;
for(const key of ['frames','photonsPerFrame','transportHash','rrHistoryResets','fluidHistoryResets'])
  assert.equal(a.report[key],b.report[key],`Mismatched ${key}`);
if(repeated)for(const key of ['frames','photonsPerFrame','transportHash'])
  assert.equal(a.report[key],repeated.report[key],`Mismatched baseline repeat ${key}`);
assert.equal(a.report.fluid.particles,b.report.fluid.particles);
assert.equal(a.report.fluid.steps,b.report.fluid.steps);
assert.equal(b.report.complexity.validated,true);
assert.equal(b.report.complexity.invalidMetrics,0);
const volumeChange=b.report.fluidSurface.tetrahedralVolumeEstimate/a.report.fluidSurface.tetrahedralVolumeEstimate-1;
assert.ok(Math.abs(volumeChange)<.002,'Observer changed reconstructed volume');
// Dynamic bin/scatter ordering is not bit deterministic. Bound relative L1
// disagreement; exact optical parity is additionally covered by static fixtures.
// The 96-frame moving-fluid baseline repeat differs by 1.61% in irradiance
// (history age excluded). A 3% hard cap plus the optional measured repeat guard
// separates that bin-order sensitivity from an observer-induced regression.
const limits=[.02,.02,.0001,.005,.002,.005,.05,.01,.001,.03];
const channels=a.channels.map((channel,index)=>{
  const other=b.channels[index];assert.deepEqual(channel.header,other.header);
  if(repeated)assert.deepEqual(channel.header,repeated.channels[index].header);
  let error=0,total=0,repeatError=0;
  for(let i=0;i<channel.values.length;i++){
    // Exclude atlas history age from the irradiance comparison denominator.
    if(index>=8&&i%4===3)continue;
    error+=Math.abs(channel.values[i]-other.values[i]);total+=Math.abs(channel.values[i]);
    if(repeated)repeatError+=Math.abs(channel.values[i]-repeated.channels[index].values[i]);
  }
  const relativeL1=error/Math.max(total,1e-30);
  assert.ok(relativeL1<limits[index],`Channel ${index} disagreement ${relativeL1}`);
  const repeatRelativeL1=repeated?repeatError/Math.max(total,1e-30):undefined;
  if(repeated)assert.ok(relativeL1<=Math.max(.00001,2*repeatRelativeL1),`Channel ${index} exceeds repeatability guard`);
  return {index,relativeL1,limit:limits[index],repeatRelativeL1};
});
console.log(JSON.stringify({reference,candidate,volumeChange,channels,complexity:b.report.complexity},null,2));
console.log('PASS: finite guides, optical energy, unchanged simulation accounting, bounded render disagreement.');
