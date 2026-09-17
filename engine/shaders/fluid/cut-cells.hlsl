#include "bulk-numeric.hlsli"
cbuffer CutFrame : register(b0){
    float4 MinimumCell,Maximum;
    uint4 Fine,Coarse;
    uint4 Control; // reset, collider count, fixture, flags: bulk=1, changed=2, kernel=4, uncached=8, time=16
    float4 Fixture; // plane equation; sphere center/radius for fixture 4
};
cbuffer CutCamera : register(b1){row_major float4x4 CutViewProjection;};
#include "solid.hlsli"
#include "cut-fractions.hlsli"
#include "cut-time.hlsli"
#include "density-kernel.hlsli"
StructuredBuffer<Bulk4> BulkInventory : register(t3);
RWStructuredBuffer<float> CornerPhi : register(u0);
RWStructuredBuffer<float2> FineVolume : register(u1); // current/previous open m^3
RWStructuredBuffer<float> FineArea : register(u2); // open area m^2, one value per shared face
RWStructuredBuffer<Bulk2> CoarseVolume : register(u3);
RWStructuredBuffer<float> CoarseArea : register(u4);
struct CutMetrics {float4 volume,inventory;uint4 fineCount,coarseCount,kernel;};
RWStructuredBuffer<CutMetrics> CutPartials : register(u5);
RWStructuredBuffer<CutMetrics> CutStatistics : register(u6);
RWStructuredBuffer<FluidSolidKernel> SolidKernel : register(u7);
RWStructuredBuffer<uint> KernelWork : register(u8); // count, compact IDs, per-voxel uint2 fingerprints
RWStructuredBuffer<float> PreviousCornerPhi : register(u9);
RWStructuredBuffer<float> TimeArea : register(u10);
uint cutIndex(uint3 p,uint3 n){return (p.z*n.y+p.y)*n.x+p.x;}
uint3 cutCoord(uint id,uint3 n){return uint3(id%n.x,(id/n.x)%n.y,id/(n.x*n.y));}
uint cutStride(uint3 n){return (n.x+1)*(n.y+1)*(n.z+1);}
uint cutFace(uint3 p,uint a,uint3 n){return a*cutStride(n)+cutIndex(p,n+1);}
float3 cutPosition(uint3 p){return min(Maximum.xyz,MinimumCell.xyz+float3(p)*MinimumCell.w);}
float3 cutWidths(uint3 p,uint scale){
    return max(0,min(scale*MinimumCell.w,Maximum.xyz-(MinimumCell.xyz+float3(p)*(scale*MinimumCell.w))));
}
[numthreads(128,1,1)]void CutCorners(uint id:SV_DispatchThreadID){
    if(id>=cutStride(Fine.xyz))return;float3 p=cutPosition(cutCoord(id,Fine.xyz+1));float phi=1e6;
    if(Control.z==4)phi=length(p-Fixture.xyz)-Fixture.w;
    else if(Control.z)phi=dot(float4(p,1),Fixture);
    else for(uint k=0;k<Control.y;++k)phi=min(phi,colliderPhi(Colliders[k],p));
    if(Control.w&16)PreviousCornerPhi[id]=Control.x?phi:CornerPhi[id];
    CornerPhi[id]=phi;
}
[numthreads(128,1,1)]void CutVolumes(uint id:SV_DispatchThreadID){
    if(id>=Fine.w)return;uint3 p=cutCoord(id,Fine.xyz);float phi[8];
    [unroll]for(uint i=0;i<8;++i)phi[i]=CornerPhi[cutIndex(p+uint3(i&1,(i>>1)&1,i>>2),Fine.xyz+1)];
    float3 w=cutWidths(p,1);float cap=w.x*w.y*w.z,v=cutCube(phi)*w.x*w.y*w.z;
    FineVolume[id]=float2(v,Control.x?v:FineVolume[id].x);
    if(Control.w&4){
        uint type=v>=cap*(1-1e-6)?0:(v==0?1:2);
        uint2 hash=densityKernelHash(uint2(2166136261u,0x9e3779b9u),type);
        if(type==2)[unroll]for(uint i=0;i<8;++i)hash=densityKernelHash(hash,asuint(phi[i]));
        uint offset=1+Fine.w+2*id;KernelWork[offset]=hash.x;KernelWork[offset+1]=hash.y;
    }
}
[numthreads(128,1,1)]void CutAreas(uint id:SV_DispatchThreadID){
    uint stride=cutStride(Fine.xyz);if(id>=3*stride)return;
    uint a=id/stride;uint3 p=cutCoord(id%stride,Fine.xyz+1),extent=Fine.xyz;extent[a]++;
    if(any(p>=extent)){FineArea[id]=0;if(Control.w&16)TimeArea[id]=0;return;}
    uint b=(a+1)%3,c=(a+2)%3;float phi[4];
    [unroll]for(uint i=0;i<4;++i){uint3 q=p;q[b]+=i&1;q[c]+=i>>1;phi[i]=CornerPhi[cutIndex(q,Fine.xyz+1)];}
    float fraction=.5*(cutTriangle(float3(phi[0],phi[1],phi[3]))+cutTriangle(float3(phi[0],phi[2],phi[3])));
    float3 w=cutWidths(p,1);FineArea[id]=fraction*w[b]*w[c];
    if(Control.w&16){float4 old;
        [unroll]for(uint i=0;i<4;++i){uint3 q=p;q[b]+=i&1;q[c]+=i>>1;old[i]=PreviousCornerPhi[cutIndex(q,Fine.xyz+1)];}
        TimeArea[id]=cutTimeFace(old,float4(phi[0],phi[1],phi[2],phi[3]))*w[b]*w[c];
    }
}
[numthreads(1,1,1)]void CutKernelClear(){KernelWork[0]=0;}
[numthreads(64,1,1)]void CutKernelClassify(uint id:SV_DispatchThreadID){
    if(id>=Fine.w)return;int3 center=int3(cutCoord(id,Fine.xyz));
    uint2 hash=uint2(2166136261u,0x9e3779b9u);
    // Only zero-crossing geometry can affect the kernel. SDF distances far
    // inside air/solid may change with a moving body but have identical measure.
    [loop]for(int hz=-1;hz<=1;++hz)[loop]for(int hy=-1;hy<=1;++hy)[loop]for(int hx=-1;hx<=1;++hx){
        int3 cell=center+int3(hx,hy,hz);if(any(cell<0)||any(cell>=int3(Fine.xyz)))continue;
        uint offset=1+Fine.w+2*cutIndex(cell,Fine.xyz);
        hash=densityKernelHash(hash,KernelWork[offset]);hash=densityKernelHash(hash,KernelWork[offset+1]);
    }
    if(!Control.x&&!(Control.w&8)&&all(SolidKernel[id].fingerprint==hash)){SolidKernel[id].rebuilt=0;return;}
    SolidKernel[id].fingerprint=hash;SolidKernel[id].rebuilt=1;
    uint count=WaveActiveCountBits(true),offset=WavePrefixCountBits(true),first=0;
    if(WaveIsFirstLane())InterlockedAdd(KernelWork[0],count,first);
    first=WaveReadLaneFirst(first);KernelWork[1+first+offset]=id;
}
[numthreads(1,1,1)]void CutKernelPrepare(){
    // Capacity-reduction scratch is unused until finishFrame. Reuse its first
    // uint3 for DISPATCH arguments; no extra allocator or argument buffer.
    CutPartials[0].fineCount=uint4((KernelWork[0]+63)/64,1,1,0);
}
[numthreads(64,1,1)]void CutSolidKernel(uint work:SV_DispatchThreadID){
    if(work>=KernelWork[0])return;uint id=KernelWork[1+work];int3 center=int3(cutCoord(id,Fine.xyz));
    float3 maximum=(Maximum.xyz-MinimumCell.xyz)/MinimumCell.w;
    float inside=1;
    [unroll]for(uint a=0;a<3;++a)inside*=densityKernelIntegral(-float(center[a])-.5,maximum[a]-float(center[a])-.5);
    float mass=1-inside; // The physical domain exterior, including clipped last cells.
    [loop]for(int z=-1;z<=1;++z)[loop]for(int y=-1;y<=1;++y)[loop]for(int x=-1;x<=1;++x){
        int3 cell=center+int3(x,y,z);if(any(cell<0)||any(cell>=int3(Fine.xyz)))continue;
        float3 physicalWidth=cutWidths(uint3(cell),1),width=physicalWidth/MinimumCell.w,offset=float3(x,y,z)-.5;
        float cap=physicalWidth.x*physicalWidth.y*physicalWidth.z;
        float v=FineVolume[cutIndex(cell,Fine.xyz)].x;
        if(v>=cap*(1-1e-6))continue;
        if(v==0){float full=1;[unroll]for(uint a=0;a<3;++a)full*=densityKernelIntegral(offset[a],offset[a]+width[a]);mass+=full;continue;}
        float phi[8];[unroll]for(uint i=0;i<8;++i)phi[i]=CornerPhi[cutIndex(uint3(cell)+uint3(i&1,(i>>1)&1,i>>2),Fine.xyz+1)];
        mass+=densitySolidKernel(offset,width,phi);
    }
    SolidKernel[id].mass=saturate(mass);
}
[numthreads(128,1,1)]void CutRestrictVolumes(uint id:SV_DispatchThreadID){
    if(id>=Coarse.w)return;uint3 base=cutCoord(id,Coarse.xyz)*2;Bulk2 v=0;
    [unroll]for(uint i=0;i<8;++i){uint3 p=base+uint3(i&1,(i>>1)&1,i>>2);if(all(p<Fine.xyz))v+=FineVolume[cutIndex(p,Fine.xyz)];}
    CoarseVolume[id]=v;
}
[numthreads(128,1,1)]void CutRestrictAreas(uint id:SV_DispatchThreadID){
    uint stride=cutStride(Coarse.xyz);if(id>=3*stride)return;
    uint a=id/stride;uint3 p=cutCoord(id%stride,Coarse.xyz+1),extent=Coarse.xyz;extent[a]++;
    if(any(p>=extent)){CoarseArea[id]=0;return;}
    uint b=(a+1)%3,c=(a+2)%3;float area=0;
    // The odd terminal coarse face is at Fine[a], not 2*Coarse[a].
    uint3 base=p*2;base[a]=min(base[a],Fine[a]);
    [unroll]for(uint i=0;i<4;++i){uint3 q=base;q[b]+=i&1;q[c]+=i>>1;if(q[b]<Fine[b]&&q[c]<Fine[c])area+=FineArea[cutFace(q,a,Fine.xyz)];}
    CoarseArea[id]=area;
}
groupshared CutMetrics CutShared[128];
CutMetrics cutMerge(CutMetrics a,CutMetrics b){
    a.volume.xyz+=b.volume.xyz;a.volume.w=max(a.volume.w,b.volume.w);
    a.inventory.xyz+=b.inventory.xyz;a.inventory.w=max(a.inventory.w,b.inventory.w);
    a.fineCount+=b.fineCount;a.coarseCount+=b.coarseCount;a.kernel+=b.kernel;return a;
}
void cutReduce(uint lane){GroupMemoryBarrierWithGroupSync();
    for(uint s=64;s;s/=2){if(lane<s)CutShared[lane]=cutMerge(CutShared[lane],CutShared[lane+s]);GroupMemoryBarrierWithGroupSync();}}
[numthreads(128,1,1)]void CutReduce(uint id:SV_DispatchThreadID,uint group:SV_GroupID,uint lane:SV_GroupIndex){
    CutMetrics m=(CutMetrics)0;
    if(id<Coarse.w){
        uint3 p=cutCoord(id,Coarse.xyz);float3 w=cutWidths(p,2);float capacity=w.x*w.y*w.z;
        Bulk2 v=CoarseVolume[id];m.volume.xy=float2(v);
        float delta=(Control.w&2)?float(abs(v.x-v.y)):0;m.volume.zw=delta;
        m.coarseCount=uint4(v.x>0&&v.x<capacity*(1-1e-6),v.x==0,v.x>=capacity*(1-1e-6),any(!isfinite(float2(v)))||any(v<0)||any(v>capacity*(1+1e-5)));
        if(Control.w&1){BulkScalar q=BulkInventory[id].w;
            // Report capacity mismatch; never discard or reconcile inventory.
            m.inventory=float4(q,max(0,q-v.x),v.x==0?q:0,v.x>1e-12?q/v.x:0);
        }
        [unroll]for(uint i=0;i<8;++i){uint3 c=p*2+uint3(i&1,(i>>1)&1,i>>2);if(any(c>=Fine.xyz))continue;
            float3 size=cutWidths(c,1);float cap=size.x*size.y*size.z;float2 f=FineVolume[cutIndex(c,Fine.xyz)];
            m.fineCount+=uint4(f.x>0&&f.x<cap*(1-1e-6),f.x==0,f.x>=cap*(1-1e-6),any(!isfinite(f))||any(f<0)||any(f>cap*(1+1e-5)));
            if(Control.w&4){FluidSolidKernel k=SolidKernel[cutIndex(c,Fine.xyz)];
                m.kernel+=uint4((Control.w&2)?k.rebuilt:0,1,0,!isfinite(k.mass)||k.mass<0||k.mass>1);}
        }
    }
    CutShared[lane]=m;cutReduce(lane);if(lane==0)CutPartials[group]=CutShared[0];
}
[numthreads(128,1,1)]void CutTotals(uint lane:SV_GroupIndex){
    CutMetrics m=(CutMetrics)0;for(uint i=lane;i<(Coarse.w+127)/128;i+=128)m=cutMerge(m,CutPartials[i]);
    CutShared[lane]=m;cutReduce(lane);if(lane==0)CutStatistics[0]=CutShared[0];
}
struct CutVertex {float4 position:SV_Position;float3 color:COLOR;};
CutVertex CutVS(uint vertex:SV_VertexID,uint id:SV_InstanceID){
    uint3 p=cutCoord(id,Coarse.xyz);float3 width=cutWidths(p,2);Bulk2 v=CoarseVolume[id];
    float fraction=float(v.x/(width.x*width.y*width.z));uint a=vertex/8,k=(vertex/2)%4;
    float3 corner=0;corner[a]=vertex&1;corner[(a+1)%3]=k&1;corner[(a+2)%3]=k>>1;
    float3 world=MinimumCell.xyz+float3(p)*(2*MinimumCell.w)+width*corner;
    CutVertex o;o.position=fraction>0&&fraction<.999999?mul(float4(world,1),CutViewProjection):float4(2,2,2,1);
    o.color=lerp(float3(1,.1,.05),float3(.05,.9,1),fraction);return o;
}
float4 CutPS(CutVertex v):SV_Target{return float4(v.color,1);}
