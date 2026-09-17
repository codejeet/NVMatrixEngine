#ifndef FLUID_INTERIOR_SHARED
#define FLUID_INTERIOR_SHARED
// Persistent dormant grid aggregate: mass/mean velocity/APIC first moment.
// A separable resting-lattice measure preserves the original transfer footprint.
// The packed dimensions are integration metadata, not live particle records.
struct InteriorCell {float4 positionRadius,velocityFlags,apic0,apic1,apic2;};
#ifndef INTERIOR_STATE_REGISTER
#define INTERIOR_STATE_REGISTER u22
#define INTERIOR_TOTAL_REGISTER u23
#endif
RWStructuredBuffer<InteriorCell> Interior : register(INTERIOR_STATE_REGISTER);
RWStructuredBuffer<uint> InteriorTotals : register(INTERIOR_TOTAL_REGISTER);
uint3 interiorDimensions(uint3 fine){return (fine+1)/2;}
uint interiorIndex(uint3 c,uint3 fine){uint3 n=interiorDimensions(fine);return (c.z*n.y+c.y)*n.x+c.x;}
uint3 interiorCoord(uint id,uint3 fine){uint3 n=interiorDimensions(fine);return uint3(id%n.x,(id/n.x)%n.y,id/(n.x*n.y));}
float3 interiorOffset(uint child,float h){return (float3(child&1,(child>>1)&1,child>>2)-.5)*h;}
float interiorQuadratic(float x){x=abs(x);return x<.5?.75-x*x:(x<1.5?.5*(1.5-x)*(1.5-x):0);}
uint3 interiorShape(InteriorCell c){uint packed=uint(c.velocityFlags.w);return uint3(packed&31,(packed>>5)&31,(packed>>10)&31);}
float3 interiorSpacing(InteriorCell c){return float3(c.positionRadius.w,c.apic1.w,c.apic2.w);}
float3 interiorMoment(InteriorCell c,float h){float3 n=float3(interiorShape(c)),s=interiorSpacing(c);return .25*h*h+s*s*(n*n-1)/12;}
float3 interiorNode(InteriorCell c,uint i){uint3 n=interiorShape(c);return (float3(i%n.x,(i/n.x)%n.y,i/(n.x*n.y))-.5*float3(n-1))*interiorSpacing(c);}
// Exact separable sum of the admitted uniform lattice's quadratic kernels.
float interiorWeight(InteriorCell c,float3 q,float h){uint3 n=interiorShape(c);float3 weights=0;
    for(uint a=0;a<3;++a)for(uint i=0;i<n[a];++i)
        weights[a]+=interiorQuadratic(q[a]-(float(i)-.5*float(n[a]-1))*interiorSpacing(c)[a]/h)/n[a];
    return weights.x*weights.y*weights.z*c.apic0.w;
}
float3 interiorVelocity(InteriorCell c,float3 d){return c.velocityFlags.xyz+float3(dot(c.apic0.xyz,d),dot(c.apic1.xyz,d),dot(c.apic2.xyz,d));}
uint interiorCellQuanta(uint3 fineCell,uint3 fine,float4 minimumCell){InteriorCell c=Interior[interiorIndex(fineCell/2,fine)];
    if(!c.velocityFlags.w)return 0;
    uint3 n=interiorShape(c);float3 spacing=interiorSpacing(c);
    float3 first=c.positionRadius.xyz-.5*float3(n-1)*spacing;
    float3 split=minimumCell.xyz+float3((fineCell/2)*2+1)*minimumCell.w;
    uint3 low=uint3(clamp(ceil((split-first)/spacing),0,float3(n)));
    uint3 count=uint3((fineCell.x&1)?n.x-low.x:low.x,(fineCell.y&1)?n.y-low.y:low.y,(fineCell.z&1)?n.z-low.z:low.z);
    return count.x*count.y*count.z*16;
}
// Gather bounds account for an aggregate centroid within .4 fine cells of its
// owning coarse center, up to one-cell lattice offsets and the 1.5-cell MAC kernel.
void interiorGatherBounds(float3 gridPosition,uint3 fine,out int3 lo,out int3 hi){
    lo=max(0,int3(ceil((gridPosition-1-2.9)/2)));
    hi=min(int3(interiorDimensions(fine))-1,int3(floor((gridPosition-1+2.9)/2)));
}
float2 interiorGather(float3 gridPosition,float4 minimumCell,uint3 fine,uint axis){
    if(!InteriorTotals[0])return 0;int3 lo,hi;interiorGatherBounds(gridPosition,fine,lo,hi);
    float2 result=0;
    for(int z=lo.z;z<=hi.z;++z)for(int y=lo.y;y<=hi.y;++y)for(int x=lo.x;x<=hi.x;++x){
        InteriorCell c=Interior[interiorIndex(uint3(x,y,z),fine)];if(!c.velocityFlags.w)continue;
        float3 d=minimumCell.xyz+gridPosition*minimumCell.w-c.positionRadius.xyz;
        float weight=interiorWeight(c,d/minimumCell.w,minimumCell.w);
        float3 row=axis==0?c.apic0.xyz:(axis==1?c.apic1.xyz:c.apic2.xyz);
        result+=float2(weight*(c.velocityFlags[axis]+dot(row,d)),weight);
    }
    return result;
}
// Negative interior extension only. Its padded boundary must lie within the
// protected particle band; no box interface is ever a visible water surface.
float interiorPhi(float3 p,float4 minimumCell,uint3 fine){
    if(!InteriorTotals[0])return 1e6;
    float3 g=(p-minimumCell.xyz)/minimumCell.w;int3 lo=max(0,int3(floor((g-2)/2))),hi=min(int3(interiorDimensions(fine))-1,int3(floor((g+2)/2)));
    float phi=1e6;
    for(int z=lo.z;z<=hi.z;++z)for(int y=lo.y;y<=hi.y;++y)for(int x=lo.x;x<=hi.x;++x){
        if(!Interior[interiorIndex(uint3(x,y,z),fine)].velocityFlags.w)continue;
        float3 q=abs(g-(float3(x,y,z)*2+1))-2;
        phi=min(phi,(length(max(q,0))+min(max(q.x,max(q.y,q.z)),0))*minimumCell.w);
    }
    return phi;
}
#endif
