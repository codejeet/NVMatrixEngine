cbuffer BuoyancyFrame:register(b0) {
    float4 FluidMinimumSpacing,DomainMinimum,DomainMaximum;
    uint4 FluidBricks,Grid,Control;
};
#include "field.hlsli"
StructuredBuffer<float4> Queries:register(t0);
RWStructuredBuffer<float4> Samples:register(u0);
RWStructuredBuffer<float4> Faces:register(u1);
float3 flow(float3 p) {
    uint stride=(Grid.x+1)*(Grid.y+1)*(Grid.z+1);float3 velocity=0;
    [unroll]for(uint axis=0;axis<3;++axis) {
        float3 offset=.5;offset[axis]=0;
        float3 g=(p-DomainMinimum.xyz)/DomainMinimum.w-offset;
        int3 base=int3(floor(g));float3 f=frac(g);float v=0,w=0;
        [unroll]for(uint k=0;k<8;++k) {
            int3 bit=int3(k&1,(k>>1)&1,k>>2),cell=base+bit,extent=int3(Grid.xyz);extent[axis]++;
            if(any(cell<0)||any(cell>=extent))continue;
            float4 face=Faces[axis*stride+(cell.z*(Grid.y+1)+cell.y)*(Grid.x+1)+cell.x];
            float3 weight=lerp(1-f,f,float3(bit));float a=weight.x*weight.y*weight.z*face.w;
            v+=a*face.x;w+=a;
        }
        velocity[axis]=w>1e-5?v/w:0;
    }
    return velocity;
}
float height(float2 xz) {
    if(any(xz<DomainMinimum.xz)||any(xz>=DomainMaximum.xz))return -1e6;
    float step=FluidMinimumSpacing.w*.5,previous=DomainMinimum.y;
    bool connected=false;
    // Hydrostatic head belongs to the basin-connected body of water, NOT the
    // highest detached droplet above a hull. Search upward from the floor and
    // stop at the first free surface; optical splashes remain fully rendered.
    for(float y=DomainMinimum.y+step;y<=DomainMaximum.y;y+=step) {
        float3 p=float3(xz.x,y,xz.y);
        float phi=liquidSample(p).x;
        if(!connected) {
            if(y>DomainMinimum.y+FluidMinimumSpacing.w*3)return -1e6;
            if(phi<=0 && liquidSample(p+float3(0,step,0)).x<=0)connected=true;
        } else if(phi>0) {
            float wet=previous,air=y;
            [unroll]for(uint k=0;k<6;++k) {
                float middle=(wet+air)*.5;
                if(liquidSample(float3(xz.x,middle,xz.y)).x<=0)wet=middle;else air=middle;
            }
            return (wet+air)*.5;
        }
        previous=y;
    }
    return connected?DomainMaximum.y:-1e6;
}
[numthreads(32,1,1)]
void BuoyancySample(uint id:SV_DispatchThreadID) {
    if(id>=Control.x)return;
    float3 p=Queries[id].xyz;float sum=0,weight=0;
    [unroll]for(uint i=0;i<4;++i) {
        float2 offset=float2(i&1?1:-1,i&2?1:-1)*FluidMinimumSpacing.w*.25;
        float h=height(p.xz+offset);
        if(h>-1e5){sum+=h;weight++;}
    }
    float h=weight>0?sum/weight:-1e6;
    Samples[id]=float4(h,weight>0?flow(float3(p.x,h-FluidMinimumSpacing.w,p.z)):0);
}
