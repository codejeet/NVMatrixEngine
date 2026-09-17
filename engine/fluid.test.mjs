import test from 'node:test';
import assert from 'node:assert/strict';
test('cached pressure stencils preserve free-surface ghosts and solid Neumann boundaries',()=>{
  // Independent one-cell reference, exhaustive air/liquid/solid six-face types.
  // Air includes sigma*kappa ghost pressure, solids contribute no diagonal.
  for(let code=0;code<3**6;code++){
    let digits=code,diagonal=0,mask=0,ghost=0,reference=0;
    const p=[.2,4.1,-.7,1.9,0,2.3],rhs=.73;
    for(let face=0;face<6;face++){
      const type=digits%3;digits=Math.floor(digits/3);
      if(type===2)continue;
      diagonal++;
      if(type===1){mask|=1<<face;reference+=p[face];}
      else {const boundary=(face-2)*.013;ghost+=boundary;reference+=boundary;}
    }
    let sum=0;for(let face=0;face<6;face++)if(mask&(1<<face))sum+=p[face];
    const assembled=(sum+ghost-rhs)*(diagonal?1/diagonal:0);
    assert.ok(Math.abs(assembled-(diagonal?(reference-rhs)/diagonal:0))<1e-14);
  }
});
const q=x=>{x=Math.abs(x);return x<.5?.75-x*x:x<1.5?.5*(1.5-x)**2:0;};
test('quadratic MAC weights reproduce constants, linear fields and APIC moment',()=>{
  for(const offset of [0,.5])for(let i=0;i<=1000;i++){
    const p=i/1000+2.001;let sum=0,first=0,second=0,affine=0;
    for(let j=-2;j<=8;j++){
      const x=j+offset,w=q(x-p);sum+=w;first+=w*(x-p);second+=w*(x-p)**2;affine+=w*(.7+.31*x);
    }
    assert.ok(Math.abs(sum-1)<1e-14);assert.ok(Math.abs(first)<1e-14);
    assert.ok(Math.abs(second-.25)<1e-14);assert.ok(Math.abs(affine-(.7+.31*p))<1e-14);
  }
});
test('staggered gather bin bounds include the full compact kernel support',()=>{
  for(const offset of [0,.5])for(let c=0;c<8;c++)for(let i=0;i<1000;i++){
    const face=c+offset,p=i/100;if(!q(p-face))continue;
    const cell=Math.floor(p),lo=Math.floor(face-1.5),hi=Math.ceil(face+1.5)-1;
    assert.ok(cell>=lo&&cell<=hi);
  }
});
test('block exclusive scan handles empty and non-power-of-two tails',()=>{
  for(const n of [1,255,256,257,105750]){
    const counts=Array.from({length:n},(_,i)=>(i*17)%23),offsets=[],blocks=[];
    for(let i=0;i<n;i+=256){let sum=0;for(let j=i;j<Math.min(i+256,n);j++){offsets[j]=sum;sum+=counts[j];}blocks.push(sum);}
    let total=0;for(let i=0;i<blocks.length;i++){let sum=blocks[i];blocks[i]=total;total+=sum;}
    let expected=0;for(let i=0;i<n;i++){offsets[i]+=blocks[Math.floor(i/256)];assert.equal(offsets[i],expected);expected+=counts[i];}
    assert.equal(total,expected);
  }
});

test('MAC projection has compatible closed walls and zero-pressure free surfaces',()=>{
  const n=8,count=n**3,index=(x,y,z)=>(z*n+y)*n+x;
  const coords=id=>[id%n,Math.floor(id/n)%n,Math.floor(id/n**2)];
  for(const closed of [false,true]){
    const liquid=Array.from({length:count},(_,id)=>closed||coords(id).every(x=>x>=2&&x<=5));
    const faces=Array.from({length:3},()=>new Float64Array((n+1)**3));
    const face=(p,a)=>{const q=[...p];return (q[2]*(n+1)+q[1])*(n+1)+q[0];};
    for(let a=0;a<3;a++)for(let z=0;z<n+1;z++)for(let y=0;y<n+1;y++)for(let x=0;x<n+1;x++){
      const p=[x,y,z];faces[a][face(p,a)]=(p[a]===0||p[a]===n)?0:Math.sin(p[a]*Math.PI/n);
    }
    const div=id=>{const p=coords(id);return faces.reduce((d,f,a)=>{const next=[...p];next[a]++;return d+f[face(next,a)]-f[face(p,a)];},0);};
    const rhs=Float64Array.from({length:count},(_,id)=>liquid[id]?div(id):0);
    if(closed)assert.ok(Math.abs(rhs.reduce((a,b)=>a+b,0))<1e-12);
    let pressure=new Float64Array(count),other=new Float64Array(count);
    for(let iteration=0;iteration<500;iteration++){
      for(let id=0;id<count;id++){
        if(!liquid[id])continue;const p=coords(id);let sum=0,diagonal=0;
        for(let a=0;a<3;a++)for(const side of [-1,1]){const q=[...p];q[a]+=side;if(q[a]<0||q[a]>=n)continue;sum+=pressure[index(...q)];diagonal++;}
        other[id]=(sum-rhs[id])/diagonal;
      }
      [pressure,other]=[other,pressure];
    }
    for(let id=0;id<count;id++)for(let a=0;a<3;a++){
      const right=coords(id),left=[...right];left[a]--;if(left[a]<0)continue;
      const l=index(...left);if(liquid[id]||liquid[l])faces[a][face(right,a)]-=pressure[id]-pressure[l];
    }
    const before=rhs.reduce((a,b)=>a+b*b,0);
    const after=liquid.reduce((a,b,id)=>a+(b?div(id)**2:0),0);
    assert.ok(after<before*1e-10,`${closed?'closed':'free'} projection ${after}/${before}`);
  }
});

test('APIC G2P affine moments and FLIP constant acceleration',()=>{
  const v=.7,slope=.31,h=.08,dt=1/120,g=-9.81;
  for(const offset of [0,.5])for(let i=0;i<100;i++){
    const p=(3+i/100)*h;let pic=0,delta=0,moment=0;
    for(let cell=0;cell<8;cell++){
      const x=(cell+offset)*h,w=q((x-p)/h),old=v+slope*x,updated=old+g*dt;
      pic+=w*updated;delta+=w*(updated-old);moment+=w*updated*(x-p);
    }
    assert.ok(Math.abs(pic-(v+slope*p+g*dt))<1e-12);
    assert.ok(Math.abs(moment*4/h**2-slope)<1e-12);
    for(const flip of [0,.5,.95,1])assert.ok(Math.abs((1-flip)*pic+flip*(v+slope*p+delta)-(v+slope*p+g*dt))<1e-12);
  }
});

test('density projection expands compression and fills only missing solid kernel support',()=>{
  // Numerical integral independently checks the flat-wall correction in HLSL.
  let missing=0;const dx=1e-5;
  for(let x=-1.5+dx/2;x<-.5;x+=dx)missing+=q(x)*dx;
  assert.ok(Math.abs(missing-1/6)<1e-10);
  const n=16,h=.08,rhs=Array.from({length:n},(_,i)=>i>0&&i<n-1?.025:0);
  let p=new Float64Array(n),next=new Float64Array(n);
  for(let k=0;k<2000;k++){
    for(let i=1;i<n-1;i++)next[i]=.5*(p[i-1]+p[i+1]+h*h*rhs[i]);
    [p,next]=[next,p];
  }
  const displacement=Float64Array.from({length:n-1},(_,i)=>-(p[i+1]-p[i])/h);
  assert.ok(displacement[0]<0&&displacement[n-2]>0);
  for(let i=1;i<n-1;i++)assert.ok(Math.abs((displacement[i]-displacement[i-1])/h-rhs[i])<1e-10);
  const error=density=>Math.min(Math.max(density-1,0),.5);
  for(const density of [0,.1,.99,1])assert.equal(error(density),0);
  assert.ok(Math.abs(error(1.1)-.1)<1e-12,'Solve full compression error within trust region');
  for(const density of [1.5,2,10])assert.equal(error(density),.5,'Bound large-error linearization');
});

test('density repair dispatch preserves globally coupled solve and skips empty work',()=>{
  for(const cells of [1,257,105750])for(const particles of [1,100000,1000000]){
    const groups=[Math.ceil(cells/256),Math.ceil(particles/256),Math.ceil(cells/256),1,
      Math.ceil(cells/256),Math.ceil(particles/256),Math.ceil(cells/256),Math.ceil(particles/128)];
    for(const rho of [0,1,1.019,1.021,2])for(const occupied of [false,true])for(const solid of [false,true]){
      const requested=occupied&&!solid&&rho>1.02;
      const args=groups.flatMap(n=>[requested?n:0,1,1]);
      assert.equal(args.length,24);
      assert.ok(args.filter((_,i)=>i%3===0).every((x,i)=>x===(requested?groups[i]:0)));
      assert.ok(args.every((x,i)=>i%3===0||x===1),'All dispatch Y/Z dimensions stay valid');
    }
  }
});

test('paired density dispatch covers long thin domains within DX12 group limits',()=>{
  for(const count of [1,65534,65535,65536,250000,1048576]){
    const x=Math.min(count,65535),y=Math.ceil(count/65535),seen=new Uint8Array(count);
    assert.ok(x<=65535&&y<=65535);
    for(let row=0;row<y;row++)for(let column=0;column<x;column++){
      const id=column+row*65535;if(id<count)seen[id]++;
    }
    assert.ok(seen.every(n=>n===1));
  }
});

test('paired density tiles equal two global sweeps across partial tiles and solid/air boundaries',()=>{
  const f=Math.fround;
  for(const g of [[1,1,1],[9,5,7],[17,9,11]]){
    const n=g.reduce((a,b)=>a*b),index=p=>(p[2]*g[1]+p[1])*g[0]+p[0];
    const coord=id=>[id%g[0],Math.floor(id/g[0])%g[1],Math.floor(id/(g[0]*g[1]))];
    const inside=p=>p.every((x,a)=>x>=0&&x<g[a]);
    const type=Array.from({length:n},(_,i)=>i%13===0?2:i%7===0?0:1);
    const stencil=Array.from({length:n},(_,id)=>{
      let mask=0,diagonal=0;const p=coord(id);
      if(type[id]===1)for(let a=0;a<3;a++)for(const s of [-1,1]){
        const q=[...p];q[a]+=s;if(!inside(q)||type[index(q)]===2)continue;
        diagonal++;if(type[index(q)]===1)mask|=1<<(a*2+(s>0?1:0));
      }
      return {mask,inv:diagonal?f(1/diagonal):0,rhs:f(.001*(id%19))};
    });
    const update=(id,read)=>{
      const p=coord(id),s=stencil[id];let sum=0;
      for(let a=0;a<3;a++)for(const side of [-1,1])if(s.mask&(1<<(a*2+(side>0?1:0)))){
        const q=[...p];q[a]+=side;sum=f(sum+read(q));
      }
      return f(f(sum+s.rhs)*s.inv);
    };
    const sweep=input=>Float32Array.from({length:n},(_,id)=>update(id,p=>input[index(p)]));
    const paired=input=>{
      const out=new Float32Array(n);
      for(let z=0;z<g[2];z+=4)for(let y=0;y<g[1];y+=4)for(let x=0;x<g[0];x+=8){
        const halo=new Map();
        for(let k=z-1;k<z+5;k++)for(let j=y-1;j<y+5;j++)for(let i=x-1;i<x+9;i++){
          const p=[i,j,k];halo.set(p.join(','),inside(p)?update(index(p),q=>input[index(q)]):0);
        }
        for(let k=z;k<Math.min(z+4,g[2]);k++)for(let j=y;j<Math.min(y+4,g[1]);j++)for(let i=x;i<Math.min(x+8,g[0]);i++){
          const p=[i,j,k];out[index(p)]=update(index(p),q=>halo.get(q.join(',')));
        }
      }
      return out;
    };
    for(const iterations of [1,2,3,7,8]){
      let scalar=Float32Array.from({length:n},(_,i)=>type[i]===1?f(Math.sin(i)) : 0),tile=scalar.slice();
      for(let i=0;i<iterations;i++)scalar=sweep(scalar);
      for(let i=0;i<iterations;){const pair=i+1<iterations;tile=pair?paired(tile):sweep(tile);i+=pair?2:1;}
      assert.deepEqual(tile,scalar,`${g} / ${iterations} sweeps`);
    }
  }
});
