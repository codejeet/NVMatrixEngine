// Read-only evidence collector; persist its checked output with apply_patch.
import {readFileSync,statSync} from 'node:fs';
import {join} from 'node:path';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
import {readCapture} from './validate-captures.mjs';
const folder=process.argv[2];
if(!folder||process.argv.length!==3)throw Error('Usage: node engine/record-fluid-owned-particles.mjs RUNTIME');
const hash=p=>createHash('sha256').update(readFileSync(p)).digest('hex');
const built=statSync(join(folder,'NVMatrixFluidLab.exe')).mtimeMs;
const read=name=>{
 const path=join(folder,name+'.json');assert.ok(statSync(path).mtimeMs>=built,`Stale ${name}`);
 return {r:JSON.parse(readFileSync(path,'utf8')),sha256:hash(path)};
};
const compareDisplayed=(reference,candidate)=>{
 // Renderer::capture copies the presented backbuffer, including RmlUi. The
 // wall-clock FPS text is intentionally different between separate launches.
 // At this fixture's 960x540/1dp scale, lab.rcss places that small widget at
 // right:18, top:13, font-size:12, padding:3 7. Nothing else is masked.
 const a=readFileSync(reference),b=readFileSync(candidate);
 const header=/^P6\n960 540\n255\n/.exec(a.toString('ascii',0,32));assert.ok(header);
 const start=header[0].length;assert.ok(a.subarray(0,start).equals(b.subarray(0,start)));
 assert.equal(a.length,start+960*540*3);assert.equal(a.length,b.length);
 let overlayPixels=0;
 for(let p=0;p<960*540;p++){
  const i=start+3*p;if(a[i]===b[i]&&a[i+1]===b[i+1]&&a[i+2]===b[i+2])continue;
  const x=p%960,y=Math.floor(p/960);
  assert.ok(x>=880&&x<942&&y>=13&&y<34,`Displayed scene changed at ${x},${y}`);
  overlayPixels++;
 }
 return {sceneAndOtherUiBitIdentical:true,excludedOverlay:'#fps',excludedRectangle:[880,13,942,34],differentFpsPixels:overlayPixels};
};
const cases=[];
for(const [name,frames] of [['empty',4],['short',8],['affine',1],['flip',32],['inlet',64],['mixed',64],['reset',180]]){
 const {r,sha256}=read('owned-particles-'+name),f=r.fluid,a=f.particleAuthority;
 assert.equal(r.frames,frames);assert.ok(f.ownedParticles&&f.validated&&a.validated);
 assert.equal(a.inventoryBits,64);assert.equal(a.invalid,0);assert.equal(f.droppedSeconds,0);
 assert.ok(a.relativeVolumeError<=1e-11&&a.massCacheRelativeError<=2e-7&&a.velocityCacheRelativeError<=3e-7);
 if(name==='mixed'){
  assert.ok(f.adaptiveMac.validated&&f.cutCells.validated&&f.work.validated);
  assert.equal(f.work.sameStateFaceMaxDifference,0);assert.equal(f.work.sameStateDensityMaxDifference,0);
 }
 const optical=name==='affine'?null:readCapture(join(folder,'owned-particles-'+name)).summary;
 if(name!=='affine')assert.equal(r.dlssEvaluations,frames);
 cases.push({name,frames,reportSha256:sha256,steps:f.steps,emitted:f.emittedParticles,authority:a,optical});
}
const comparisons=[];
for(const name of ['short','inlet','mixed']){
 const reference=read('owned-reference-'+name),candidate=read('owned-particles-'+name);
 assert.ok(!reference.r.fluid.ownedParticles&&reference.r.fluid.validated);
 assert.equal(reference.r.frames,candidate.r.frames);
 for(const field of ['particles','emittedParticles','steps','restVolume','postStepOccupiedVolume','meanHeight'])
  assert.equal(reference.r.fluid[field],candidate.r.fluid[field],`${name} physical ${field}`);
 const a=readFileSync(join(folder,'owned-reference-'+name+'.inputs'));
 const b=readFileSync(join(folder,'owned-particles-'+name+'.inputs'));
 assert.ok(a.equals(b),`${name}: raw lighting, guides or caustic snapshot changed`);
 const referencePpm=hash(join(folder,'owned-reference-'+name+'.ppm'));
 const candidatePpm=hash(join(folder,'owned-particles-'+name+'.ppm'));
 const displayed=compareDisplayed(join(folder,'owned-reference-'+name+'.ppm'),join(folder,'owned-particles-'+name+'.ppm'));
 comparisons.push({name,frames:candidate.r.frames,referenceReportSha256:reference.sha256,candidateReportSha256:candidate.sha256,
   inputSha256:hash(join(folder,'owned-particles-'+name+'.inputs')),referenceOutputSha256:referencePpm,outputSha256:candidatePpm,
   allRawCapturedChannelsBitIdentical:true,displayed});
}
const sourceFiles=['engine/src/fluid/fluid_system.cpp','engine/src/fluid/fluid_system.h','engine/src/fluid/fluid_uniforms.h',
 'engine/src/main.cpp','engine/src/renderer.cpp','engine/src/renderer.h','engine/src/hud.cpp','engine/ui/lab.rcss',
 'engine/src/fluid/fluid_particle_grid_exchange.cpp','engine/src/fluid/fluid_particle_grid_exchange.h',
 'engine/src/fluid/fluid_work.cpp','engine/src/fluid/fluid_work.h','engine/src/fluid/fluid_complexity.cpp',
 'engine/shaders/fluid/particle-grid-exchange.hlsl','engine/shaders/fluid/binning.hlsl','engine/shaders/fluid/transfer.hlsl',
 'engine/shaders/fluid/common.hlsli','engine/shaders/fluid/complexity.hlsl','engine/tests/fluid_exchange_gpu.cpp',
 'engine/test-fluid-owned-particles.ps1','engine/compile-fluid-exchange.ps1','engine/particle-grid-exchange.test.mjs'];
const shaderFiles=['ExchangeReset','ExchangeClear','ExchangeSeed','ExchangeVelocityDelta','ExchangeDeposit','ExchangeFree','ExchangeRestore',
 'FluidGatherMassAuthority','FluidP2GAuthority','FluidP2GSparseAuthority','FluidClearBins','FluidCountBins','FluidScanCells','FluidScanSums','FluidFinishScan','FluidScatter','ComplexityClassify'];
console.log(JSON.stringify({schema:1,generatedUtc:new Date().toISOString(),scope:'Live particle authority, fractional mass consumers; no automatic flowing grid ownership',
 adapter:read('owned-particles-short').r.adapter,
 defaultEnabled:false,performanceMeasured:false,gpuDebugLayerEnabled:false,gpuBasedValidationEnabled:false,
 executableSha256:hash(join(folder,'NVMatrixFluidLab.exe')),hardwareFixtureSha256:hash(join(folder,'NVMatrixEngineExchangeGpuTest.exe')),
 sourceSha256:Object.fromEntries(sourceFiles.map(p=>[p,hash(p)])),shaderSha256:Object.fromEntries(shaderFiles.map(p=>[p,hash(join(folder,'shaders',p+'.dxil'))])),
 cases,comparisons},null,2));
