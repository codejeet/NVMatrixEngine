// A trilinear scalar is not a distance bound: multiplying abs(phi) by a safety
// factor alone cannot guarantee finding thin sheets. Traverse exact field cells,
// split the cubic along the ray at its extrema, and refine the first root.
float liquidPolynomial(float4 c,float t){return ((c.w*t+c.z)*t+c.y)*t+c.x;}
bool liquidRoot(float4 c,out float root) {
    float cuts[4];cuts[0]=0;uint count=1;
    float a=3*c.w,b=2*c.z,d=c.y;
    if(abs(a)<1e-12) {
        if(abs(b)>1e-12){float r=-d/b;if(r>0&&r<1)cuts[count++]=r;}
    }else {
        float disc=b*b-4*a*d;
        if(disc>=0){float s=sqrt(disc),r0=(-b-s)/(2*a),r1=(-b+s)/(2*a);
            if(r0>r1){float tmp=r0;r0=r1;r1=tmp;}
            if(r0>0&&r0<1)cuts[count++]=r0;
            if(r1>0&&r1<1&&r1>r0+1e-7)cuts[count++]=r1;}
    }
    cuts[count++]=1;root=0;
    for(uint i=0;i+1<count;++i) {
        float lo=cuts[i],hi=cuts[i+1],fl=liquidPolynomial(c,lo),fh=liquidPolynomial(c,hi);
        if(abs(fl)<1e-8){root=lo;return true;}
        if((fl<0)!=(fh<0)) {
            // Sixteen refinements also bound world-space error at the large
            // room's 32cm render cells; twelve could exceed the 50um residual
            // gate even for correctly bracketed roots. Only hit cells pay this.
            [unroll]for(uint j=0;j<16;++j){float m=.5*(lo+hi),fm=liquidPolynomial(c,m);if((fm<0)==(fl<0)){lo=m;fl=fm;}else hi=m;}
            root=.5*(lo+hi);return true;
        }
        if(abs(fh)<1e-8){root=hi;return true;}
    }
    return false;
}
struct FluidAttributes {float2 unused;};
[shader("intersection")]
void FluidIntersection() {
    uint id=PrimitiveIndex(),slot=LiquidBrickMap[id];if(slot==0xffffffff)return;
    uint3 brick=uint3(id%FluidBricks.x,(id/FluidBricks.x)%FluidBricks.y,id/(FluidBricks.x*FluidBricks.y));
    float h=FluidMinimumSpacing.w;
    float3 lower=FluidMinimumSpacing.xyz+float3(brick*8)*h,upper=lower+8*h;
    // DXR culls against the tight surface AABB, but does not pass its entry T to
    // this shader. Clip the software DDA to those same conservative cell bounds.
    // This avoids marching the empty/interior part of every shallow pool brick.
    uint packedBounds=LiquidBrickMap[FluidBricks.w+slot*17+16];
    float3 clipLo=lower+float3(packedBounds&15,(packedBounds>>4)&15,(packedBounds>>8)&15)*h-h*.001;
    float3 clipHi=lower+float3((packedBounds>>12)&15,(packedBounds>>16)&15,(packedBounds>>20)&15)*h+h*.001;
    clipLo=max(clipLo,lower);clipHi=min(clipHi,upper);
    if(FluidState.w&64){clipLo=lower;clipHi=upper;} // Same-binary validation reference.
    float3 o=ObjectRayOrigin(),d=ObjectRayDirection();
    float nearT=RayTMin(),farT=RayTCurrent();
    [unroll]for(uint axis=0;axis<3;++axis) {
        if(abs(d[axis])<1e-12){if(o[axis]<clipLo[axis]||o[axis]>clipHi[axis])return;}
        else{float a=(clipLo[axis]-o[axis])/d[axis],b=(clipHi[axis]-o[axis])/d[axis];nearT=max(nearT,min(a,b));farT=min(farT,max(a,b));}
    }
    float3 domainLo,domainHi;bool phase=liquidPhaseDomain(domainLo,domainHi);
    float domainNear=-1e30,domainFar=1e30;
    if(phase){
        [unroll]for(uint axis=0;axis<3;++axis){
            if(abs(d[axis])<1e-12){if(o[axis]<domainLo[axis]||o[axis]>domainHi[axis])return;}
            else{float a=(domainLo[axis]-o[axis])/d[axis],b=(domainHi[axis]-o[axis])/d[axis];
                domainNear=max(domainNear,min(a,b));domainFar=min(domainFar,max(a,b));}
        }
        nearT=max(nearT,domainNear);farT=min(farT,domainFar);
    }
    if(nearT>farT)return;
    if(FluidState.w&1)Stats.InterlockedAdd(124,1);
    if(phase && nearT==domainNear && liquidRawSample(o+d*nearT).x<=0){
        FluidAttributes attributes;attributes.unused=0;ReportHit(nearT,0,attributes);return;
    }
    float t=nearT;
    int3 cell=clamp(int3(floor((o+d*t-lower)/h)),0,7);
    int3 direction=int3(sign(d));
    float3 nextT=1e30,deltaT=1e30;
    [unroll]for(uint a=0;a<3;++a)if(abs(d[a])>1e-12) {
        nextT[a]=(lower[a]+(cell[a]+(d[a]>0?1:0))*h-o[a])/d[a];
        deltaT[a]=h/abs(d[a]);
    }
    // A line crosses at most 3*8+1 cells inside this brick.
    for(uint step=0;step<27&&t<farT;++step) {
        if(FluidState.w&1)Stats.InterlockedAdd(120,1);
        if(any(cell<0)||any(cell>7))return;
        float3 g=(o+d*t-lower)/h;
        float endT=min(farT,min(nextT.x,min(nextT.y,nextT.z)));
        uint cellId=(cell.z*8+cell.y)*8+cell.x;
        uint mask=LiquidBrickMap[FluidBricks.w+slot*17+cellId/32];
        if(endT>t&&(mask&(1u<<(cellId%32)))) {
            float v[8];
            [unroll]for(uint k=0;k<8;++k)v[k]=liquidNode(slot,uint3(cell)+uint3(k&1,(k>>1)&1,k>>2)).x;
            float c0=v[0],cx=v[1]-v[0],cy=v[2]-v[0],cz=v[4]-v[0];
            float cxy=v[3]-v[1]-v[2]+v[0],cxz=v[5]-v[1]-v[4]+v[0],cyz=v[6]-v[2]-v[4]+v[0];
            float cxyz=v[7]-v[3]-v[5]-v[6]+v[1]+v[2]+v[4]-v[0];
            float3 p=g-float3(cell),q=d*((endT-t)/h);
            float4 c;
            c.x=c0+cx*p.x+cy*p.y+cz*p.z+cxy*p.x*p.y+cxz*p.x*p.z+cyz*p.y*p.z+cxyz*p.x*p.y*p.z;
            c.y=cx*q.x+cy*q.y+cz*q.z+cxy*(p.x*q.y+q.x*p.y)+cxz*(p.x*q.z+q.x*p.z)+cyz*(p.y*q.z+q.y*p.z)+cxyz*(q.x*p.y*p.z+p.x*q.y*p.z+p.x*p.y*q.z);
            c.z=cxy*q.x*q.y+cxz*q.x*q.z+cyz*q.y*q.z+cxyz*(q.x*q.y*p.z+q.x*p.y*q.z+p.x*q.y*q.z);
            c.w=cxyz*q.x*q.y*q.z;
            float r;if(liquidRoot(c,r)){FluidAttributes a;a.unused=0;ReportHit(lerp(t,endT,r),0,a);return;}
        }
        if(endT>=farT){t=farT;break;}
        // Advance the integer DDA state rather than re-flooring a rounded world
        // position. All tied axes advance together, including zero-length entry
        // intervals at a brick edge. No repeated-cell stall or arbitrary skip.
        [unroll]for(uint a=0;a<3;++a)if(nextT[a]<=endT+1e-7){cell[a]+=direction[a];nextT[a]+=deltaT[a];}
        t=max(t,endT);
    }
    if(phase && farT==domainFar && t>=farT && liquidRawSample(o+d*farT).x<=0){
        FluidAttributes attributes;attributes.unused=0;ReportHit(farT,0,attributes);return;
    }
    if((FluidState.w&1)&&t<farT-1e-5)Stats.InterlockedAdd(116,1);
}
[shader("closesthit")]
void FluidClosest(inout Payload p,FluidAttributes a) {
    p.t=RayTCurrent();p.primitive=PrimitiveIndex();p.object=InstanceID();p.bary=0;
}
