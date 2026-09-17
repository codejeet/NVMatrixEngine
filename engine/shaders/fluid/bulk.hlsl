// Conservative diagnostic bulk inventory. It never overwrites the MAC solver,
// particles, or optical surface. A future ownership handoff must not sum replicas.
#include "bulk-numeric.hlsli"
#ifndef BULK_PROJECTED
#define BULK_PROJECTED 0
#endif
#ifndef BULK_CAPACITY
#define BULK_CAPACITY 0
#endif
#if BULK_CAPACITY
RWStructuredBuffer<Bulk4> SourcePending : register(u17);
#endif
#if BULK_PROJECTED
#define Flow double
cbuffer BulkProjection : register(b1) { uint Swept; };
RWStructuredBuffer<double> ProjectedFlux : register(u14);
RWStructuredBuffer<float2> FineVolume : register(u15);
RWStructuredBuffer<Bulk2> CoarseVolume : register(u16);
#else
#define Flow float
#endif
cbuffer BulkFrame : register(b0) {
    float4 MinimumCell,MaximumDensity;
    uint4 FineGrid,BulkGrid;
    float4 Physical;
    uint4 Source;
    row_major float4x4 ViewProjection;
};
struct Particle {float4 positionRadius,velocityFlags,apic0,apic1,apic2;};
RWStructuredBuffer<Particle> Particles : register(u0);
RWStructuredBuffer<uint> Offsets : register(u1);
RWStructuredBuffer<uint> Indices : register(u2);
RWStructuredBuffer<float4> Faces : register(u3);
RWStructuredBuffer<float4> Solids : register(u4);
RWStructuredBuffer<Bulk4> StateIn : register(u5);
RWStructuredBuffer<Bulk4> StateOut : register(u6);
RWStructuredBuffer<Bulk4> Ledger : register(u7);
RWStructuredBuffer<Flow> Rates : register(u8); // oriented volume flow, m^3/s
RWStructuredBuffer<Bulk4> Flux : register(u9); // ONE shared transfer per face
RWStructuredBuffer<float2> Limiter : register(u10); // donor scale, outgoing CFL
struct Metrics {float4 inventory,ledger,detail;uint4 counts;};
RWStructuredBuffer<Metrics> Partials : register(u11);
RWStructuredBuffer<Metrics> Statistics : register(u12);
RWByteAddressBuffer Counters : register(u13);
uint index(uint3 p,uint3 n){return (p.z*n.y+p.y)*n.x+p.x;}
uint3 coord(uint id,uint3 n){return uint3(id%n.x,(id/n.x)%n.y,id/(n.x*n.y));}
uint stride(uint3 n){return (n.x+1)*(n.y+1)*(n.z+1);}
uint face(uint3 p,uint a,uint3 n){return a*stride(n)+index(p,n+1);}
float3 widths(uint3 p,float spacing){return max(0,min(spacing,MaximumDensity.xyz-(MinimumCell.xyz+p*spacing)));}
float volume(uint3 p){float3 w=widths(p,2*MinimumCell.w);return w.x*w.y*w.z;}
BulkScalar capacity(uint3 p){
#if BULK_PROJECTED
    return CoarseVolume[index(p,BulkGrid.xyz)].x;
#else
    return volume(p);
#endif
}
BulkScalar donorVolume(uint3 p){
#if BULK_PROJECTED
    Bulk2 v=CoarseVolume[index(p,BulkGrid.xyz)];return Swept?v.y:v.x;
#else
    return volume(p);
#endif
}
bool periodic(){return Physical.z==1||Physical.z==2;}
float3 fixtureVelocity(){return float3(.7,-.2,.3)*(Physical.z==2?100:1);}
bool faceCells(uint id,out uint3 p,out uint a,out int3 left,out int3 right){
    uint s=stride(BulkGrid.xyz);a=id/s;p=coord(id%s,BulkGrid.xyz+1);
    uint3 extent=BulkGrid.xyz;extent[a]++;
    left=int3(p);left[a]--;right=int3(p);
    return all(p<extent);
}
uint wrapped(int3 p){return index(uint3((p+int3(BulkGrid.xyz))%int3(BulkGrid.xyz)),BulkGrid.xyz);}
bool boundary(uint3 p,uint a){return p[a]==0||p[a]==BulkGrid[a];}
[numthreads(128,1,1)]
void BulkClearFrame(uint id:SV_DispatchThreadID){if(id<4)Counters.Store(id*4,0);}
[numthreads(128,1,1)]
void BulkSources(uint id:SV_DispatchThreadID){
    if(id>=BulkGrid.w)return;
    Bulk4 q=0;uint3 base=coord(id,BulkGrid.xyz)*2;
    // Each particle belongs to exactly one coarse aggregate. No truncated
    // interpolation kernel, float atomics, duplicate mass, or per-frame re-seed.
    [unroll]for(uint child=0;child<8;child++){
        uint3 p=base+uint3(child&1,(child>>1)&1,child>>2);if(any(p>=FineGrid.xyz))continue;
        uint c=index(p,FineGrid.xyz),end=Offsets[c+1];
        for(uint j=Offsets[c];j<end;j++){
            uint pid=Indices[j];if(pid<Source.x||pid-Source.x>=Source.y)continue;
            Particle v=Particles[pid];if(!v.velocityFlags.w)continue;
            float3 velocity=periodic()?fixtureVelocity():v.velocityFlags.xyz;
            q+=Bulk4(velocity,1)*(BulkScalar(Physical.y)*v.apic0.w);
        }
    }
#if BULK_CAPACITY
    if(Source.z){StateIn[id]=0;StateOut[id]=0;SourcePending[id]=q;Ledger[id]=q;Limiter[id]=float2(1,0);}
    else {SourcePending[id]+=q;Ledger[id]+=q;}
#else
    if(Source.z){StateIn[id]=q;StateOut[id]=0;Ledger[id]=q;Limiter[id]=float2(1,0);}
    else {StateIn[id]+=q;Ledger[id]+=q;}
#endif
    if(any(!isfinite(float4(q))))Counters.InterlockedAdd(0,1);
}
[numthreads(128,1,1)]
void BulkRestrictFaces(uint id:SV_DispatchThreadID){
    if(id>=3*stride(BulkGrid.xyz))return;
    uint3 p;uint a;int3 left,right;
    if(!faceCells(id,p,a,left,right)){Rates[id]=0;return;}
    uint b=(a+1)%3,c=(a+2)%3;Flow rate=0;
    if(Physical.z>0){
        float3 w=widths(p,2*MinimumCell.w);
        rate=periodic()?fixtureVelocity()[a]*w[b]*w[c]:0;
    }else if(!boundary(p,a)){
        // Sum fine volume fluxes (not an unweighted average velocity). Partial
        // final cells use physical clipped areas; internal faces cancel exactly.
        [unroll]for(uint i=0;i<2;i++)[unroll]for(uint j=0;j<2;j++){
            uint3 f=p*2;f[b]+=i;f[c]+=j;if(f[b]>=FineGrid[b]||f[c]>=FineGrid[c])continue;
#if BULK_PROJECTED
            // Already includes the physical open aperture. Do not use the
            // extrapolated FP32 velocity or multiply by aperture a second time.
            rate+=ProjectedFlux[face(f,a,FineGrid.xyz)];
#else
            uint3 l=f;l[a]--;
            if(Solids[index(l,FineGrid.xyz)].x<0||Solids[index(f,FineGrid.xyz)].x<0)continue;
            float3 w=widths(f,MinimumCell.w);
            rate+=Faces[face(f,a,FineGrid.xyz)].x*w[b]*w[c];
#endif
        }
    }
    Rates[id]=rate;
    if(!isfinite(float(rate)))Counters.InterlockedAdd(0,1);
}
[numthreads(128,1,1)]
void BulkForces(uint id:SV_DispatchThreadID){
    if(id>=BulkGrid.w||Physical.z>0)return;
    uint3 base=coord(id,BulkGrid.xyz)*2;Bulk3 delta=0;BulkScalar weight=0;
    [unroll]for(uint child=0;child<8;child++){
        uint3 p=base+uint3(child&1,(child>>1)&1,child>>2);if(any(p>=FineGrid.xyz))continue;
#if BULK_PROJECTED
        float v=FineVolume[index(p,FineGrid.xyz)].x;
#else
        if(Solids[index(p,FineGrid.xyz)].x<0)continue;
        float3 w=widths(p,MinimumCell.w);float v=w.x*w.y*w.z;
#endif
        [unroll]for(uint a=0;a<3;a++){
            uint3 r=p;r[a]++;
            float2 l=Faces[face(p,a,FineGrid.xyz)].xy,h=Faces[face(r,a,FineGrid.xyz)].xy;
            delta[a]+=.5*((l.x-l.y)+(h.x-h.y))*v;
        }
        weight+=v;
    }
    Bulk3 impulse=StateIn[id].w*delta/max(weight,1e-20);
    StateIn[id].xyz+=impulse;Ledger[id].xyz+=impulse;
    if(any(!isfinite(float3(impulse))))Counters.InterlockedAdd(0,1);
}
[numthreads(128,1,1)]
void BulkLimit(uint id:SV_DispatchThreadID){
    if(id>=BulkGrid.w)return;
    uint3 p=coord(id,BulkGrid.xyz);Flow outflow=0;
    [unroll]for(uint a=0;a<3;a++){
        uint3 r=p;r[a]++;
        outflow+=max(0,-Rates[face(p,a,BulkGrid.xyz)])+max(0,Rates[face(r,a,BulkGrid.xyz)]);
    }
    // A zero-capacity donor cannot provide physical flux. Its inventory remains
    // visible as excess, never silently discarded or divided by epsilon.
    BulkScalar v=donorVolume(p);
    Flow cfl=v>0?Flow(Physical.x)*outflow/v:0;
    float scale=v>0?(cfl>.95?float(.95/cfl):1):0;
    Limiter[id]=float2(scale,cfl);
    if(scale<1&&StateIn[id].w>0)Counters.InterlockedAdd(4,1);
}
[numthreads(128,1,1)]
void BulkFlux(uint id:SV_DispatchThreadID){
    if(id>=3*stride(BulkGrid.xyz))return;
    uint3 p;uint a;int3 left,right;
    if(!faceCells(id,p,a,left,right)||(!periodic()&&boundary(p,a))){Flux[id]=0;return;}
    Flow rate=Rates[id];uint donor=wrapped(rate>=0?left:right);
    BulkScalar v=donorVolume(coord(donor,BulkGrid.xyz));
    Flow factor=v>0?Flow(Physical.x)*rate*Limiter[donor].x/v:0;
    Flux[id]=BulkScalar(factor)*StateIn[donor];
}
[numthreads(128,1,1)]
void BulkUpdate(uint id:SV_DispatchThreadID){
    if(id>=BulkGrid.w)return;
    uint3 p=coord(id,BulkGrid.xyz);Bulk4 q=StateIn[id];
    [unroll]for(uint a=0;a<3;a++){
        uint3 r=p;r[a]++;q+=Flux[face(p,a,BulkGrid.xyz)]-Flux[face(r,a,BulkGrid.xyz)];
    }
    // No saturation or negative-volume clamp: errors must remain observable.
    StateOut[id]=q;
    if(q.w<0||any(!isfinite(float4(q))))Counters.InterlockedAdd(0,1);
}
groupshared Metrics Shared[128];
Metrics merge(Metrics a,Metrics b){
    a.inventory+=b.inventory;a.ledger+=b.ledger;a.detail.xyz=max(a.detail.xyz,b.detail.xyz);
    a.detail.w+=b.detail.w;a.counts+=b.counts;return a;
}
void groupReduce(uint lane){
    GroupMemoryBarrierWithGroupSync();
    for(uint s=64;s>0;s>>=1){if(lane<s)Shared[lane]=merge(Shared[lane],Shared[lane+s]);GroupMemoryBarrierWithGroupSync();}
}
[numthreads(128,1,1)]
void BulkReduce(uint id:SV_DispatchThreadID,uint group:SV_GroupID,uint lane:SV_GroupIndex){
    Metrics m=(Metrics)0;
    if(id<BulkGrid.w){
        m.inventory=float4(StateIn[id]);m.ledger=float4(Ledger[id]);
        BulkScalar v=capacity(coord(id,BulkGrid.xyz));
        m.detail=float4(StateIn[id].w/max(v,1e-20),
            m.inventory.w>1e-12?length(m.inventory.xyz/m.inventory.w):0,Limiter[id].y,max(0,StateIn[id].w-v));
        m.counts.x=m.inventory.w>1e-12?1:0;
        m.counts.w=m.detail.x>1.01?1:0;
        m.counts.z=any(!isfinite(m.inventory))||any(!isfinite(m.ledger))||m.inventory.w<0?1:0;
#if BULK_CAPACITY
        // Total accounting includes unadmitted requests; optical/simulation
        // consumers still see only resident StateIn, never this sum.
        Bulk4 pending=SourcePending[id];m.inventory+=float4(pending);
        m.detail.y=max(m.detail.y,pending.w>0?length(float3(pending.xyz/pending.w)):0);
        if(any(!isfinite(float4(pending)))||pending.w<0)m.counts.z++;
#endif
    }
    Shared[lane]=m;groupReduce(lane);if(lane==0)Partials[group]=Shared[0];
}
[numthreads(128,1,1)]
void BulkTotals(uint lane:SV_GroupIndex){
    Metrics m=(Metrics)0;
    for(uint i=lane;i<(BulkGrid.w+127)/128;i+=128)m=merge(m,Partials[i]);
    Shared[lane]=m;groupReduce(lane);
    if(lane==0){m=Shared[0];m.counts.y=Counters.Load(4);m.counts.z+=Counters.Load(0);Statistics[0]=m;}
}
struct DebugVertex {float4 position:SV_Position;float3 color:COLOR;};
DebugVertex BulkVS(uint vertex:SV_VertexID,uint id:SV_InstanceID){
    static const uint corners[24]={0,1,2,3,4,5,6,7,0,2,1,3,4,6,5,7,0,4,1,5,2,6,3,7};
    DebugVertex o;Bulk4 q=StateIn[id];uint3 c=coord(id,BulkGrid.xyz);uint k=corners[vertex];
    float3 lo=MinimumCell.xyz+c*(2*MinimumCell.w),hi=min(MaximumDensity.xyz,lo+2*MinimumCell.w);
    float3 p=lerp(lo,hi,float3(k&1,(k>>1)&1,k>>2));
    // Hide tiny first-order-advection tails in the wire view only. They remain
    // in the inventory, transport and conservation totals without any culling.
    o.position=q.w>.01*capacity(c)?mul(float4(p,1),ViewProjection):float4(2,2,2,1);
    float v=float(Physical.w==1?q.w/max(capacity(c),1e-20):(q.w>1e-12?length(float3(q.xyz/q.w))*.2:0));
    o.color=Physical.w==1&&v>1.01?float3(1,0,1):lerp(float3(.05,.35,1),float3(1,.15,.03),saturate(v));return o;
}
float4 BulkPS(DebugVertex i):SV_Target{return float4(i.color,1);}
