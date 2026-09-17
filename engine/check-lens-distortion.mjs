import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {pathToFileURL} from 'node:url';

function half(h) {
  const sign=h&32768?-1:1, exponent=(h>>10)&31, mantissa=h&1023;
  return exponent===31 ? NaN : sign*(exponent ? 2**(exponent-15)*(1+mantissa/1024) : 2**-14*mantissa/1024);
}
export function checkLensDistortion(prefix) {
  const report=JSON.parse(readFileSync(`${prefix}.json`,'utf8'));
  const data=readFileSync(`${prefix}.distortion`);
  assert.equal(data.subarray(0,8).toString(),'FGLENS01');
  const width=data.readUInt32LE(8), height=data.readUInt32LE(12), fov=data.readFloatLE(16);
  assert.equal(width,report.outputWidth);
  assert.equal(height,report.outputHeight);
  assert.equal(fov,report.frameGenerationDistortionFov);
  assert.equal(data.length,20+width*height*8);
  assert.ok(fov>=90&&fov<=160);
  const aspect=width/height, halfAngle=fov*Math.PI/360, diagonal=Math.hypot(aspect,1);
  const sine=Math.sin(halfAngle*.5), tangent=Math.tan(halfAngle);
  let maxPixelError=0, negative=0, positive=0, offscreen=0;
  for(let y=0;y<height;y++) for(let x=0;x<width;x++) {
    const u=(x+.5)/width, v=(y+.5)/height, qx=2*u-1, qy=2*v-1;
    const radius=Math.hypot(qx*aspect,qy)/diagonal;
    // Independent double-precision derivation from r = 2 f sin(theta/2).
    const theta=2*Math.asin(radius*sine);
    const rectScale=radius?Math.tan(theta)/(tangent*radius):2*sine/tangent;
    const fishScale=radius?Math.sin(.5*Math.atan(radius*tangent))/(sine*radius):tangent/(2*sine);
    const expected=[qx*(rectScale-1)/2,qy*(rectScale-1)/2,qx*(fishScale-1)/2,qy*(fishScale-1)/2];
    for(let c=0;c<4;c++) {
      const value=half(data.readUInt16LE(20+((y*width+x)*4+c)*2));
      assert.ok(Number.isFinite(value),'nonfinite distortion component');
      const error=Math.abs(value-expected[c]);
      assert.ok(error<=1e-6+Math.abs(expected[c])*.00051,`wrong UV displacement at ${x},${y},${c}`);
      maxPixelError=Math.max(maxPixelError,error*(c%2?height:width));
      negative+=value<0; positive+=value>0;
      const target=(c%2?v:u)+value;
      offscreen+=target<0||target>1;
    }
  }
  assert.ok(negative&&positive&&offscreen,'signed/offscreen inverse mapping was clipped');
  assert.ok(maxPixelError<.5,'distortion precision exceeds half a display pixel');
  return {width,height,fov,maxPixelError,offscreen};
}
if(process.argv[1]&&import.meta.url===pathToFileURL(process.argv[1]).href) {
  assert.ok(process.argv.length>2,'Usage: node engine/check-lens-distortion.mjs capture-prefix [...]');
  for(const prefix of process.argv.slice(2)) console.log('PASS GPU lens distortion',prefix,checkLensDistortion(prefix));
}
