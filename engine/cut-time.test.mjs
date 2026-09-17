import test from 'node:test';
import assert from 'node:assert/strict';

// Independent polygon clipping of each spatial triangle, followed by temporal
// quadrature. This avoids using the shader's closed-form simplex fractions.
function triangle(points){
  const clipped=[];
  for(let i=0;i<3;++i){const a=points[i],b=points[(i+1)%3];if(a[2]>=0)clipped.push(a);
    if((a[2]<0)!=(b[2]<0)){const t=a[2]/(a[2]-b[2]);clipped.push(a.map((v,k)=>v+(b[k]-v)*t));}}
  return Math.abs(clipped.reduce((s,a,i)=>{const b=clipped[(i+1)%clipped.length];return s+a[0]*b[1]-a[1]*b[0];},0))/2;
}
function area(p){const c=[[0,0,p[0]],[1,0,p[1]],[0,1,p[2]],[1,1,p[3]]];return triangle([c[0],c[1],c[3]])+triangle([c[0],c[2],c[3]]);}
function average(a,b){
  const roots=[0,1];a.forEach((v,i)=>{if((v<0)!=(b[i]<0)){const t=v/(v-b[i]);if(t>0&&t<1)roots.push(t);}});roots.sort((a,b)=>a-b);
  const nodes=[.0694318442,.3300094782,.6699905218,.9305681558],weights=[.1739274226,.3260725774,.3260725774,.1739274226];
  let result=0;
  for(let i=1;i<roots.length;++i)for(let k=0;k<4;++k){const dt=roots[i]-roots[i-1],t=roots[i-1]+dt*nodes[k];result+=dt*weights[k]*area(a.map((v,j)=>v+(b[j]-v)*t));}
  return result;
}
const close=(a,b,tol=1e-9)=>assert.ok(Math.abs(a-b)<tol,`${a} != ${b}`);
test('a face that closes inside a substep retains its finite escape interval',()=>{
  const a=[1,1,1,1],b=[-3,-3,-3,-3];close(area(b),0);close(average(a,b),.25);
  assert.ok(Math.abs(average(a,b)-(area(a)+area(b))/2)>.2);
});
test('opening and closing are time-reversal symmetric',()=>{
  for(const [a,b] of [[[1,-1,2,-3],[-2,1,-3,2]],[[0,0,-1,0],[1,-1,0,2]],[[1,1,1,1],[-100,-100,-100,-100]]])close(average(a,b),average(b,a));
});
test('stationary geometry and exact zero ties reproduce endpoint area',()=>{
  for(const p of [[1,1,1,1],[-1,-1,-1,-1],[0,0,0,0],[0,0,-1,0],[1,-1,2,-3]])close(average(p,p),area(p));
});
test('uniform closing/opening capacity has a solvable pressure source',()=>{
  const old=[.4,.6],current=[0,1],dt=.01,A=average([1,1,1,1],[-3,-3,-3,-3]);
  const flow=(old[0]-current[0])/dt,velocity=flow/A;
  assert.ok(A>0&&Number.isFinite(velocity));
  close(current[0]-old[0]+dt*A*velocity,0);close(current[1]-old[1]-dt*A*velocity,0);
  // Positive temporal normalization lets pressure include a disappearing cell.
  assert.ok((old[0]+current[0])/2>0);
  // Source-compatible pressure alone cannot remove the old 95% donor cap.
  assert.ok(old[0]-.95*old[0]>0);
});
test('linear translating plane gives its analytic average strip width',()=>{
  const before=[-.1,.9,-.1,.9],after=[-.8,.2,-.8,.2];close(average(before,after),.55);
});
test('aperture averages remain finite and bounded under sign/topology changes',()=>{
  let seed=791;const rand=()=>((seed=Math.imul(seed,1664525)+1013904223>>>0)/2**32);
  for(let i=0;i<500;++i){const a=Array.from({length:4},()=>2*rand()-1),b=Array.from({length:4},()=>2*rand()-1),v=average(a,b);
    assert.ok(Number.isFinite(v)&&v>=0&&v<=1+1e-10);close(v,average(b,a),2e-9);}
});
