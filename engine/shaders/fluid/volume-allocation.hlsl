// Source allocation only. Never clips/reinitializes the resident liquid field.
// Quantities are (volume-weighted velocity XYZ, rest volume W).
#include "bulk-numeric.hlsli"
cbuffer AllocationFrame : register(b0) { uint4 Grid; uint Iteration,Advance; float RoutingTolerance; };
RWStructuredBuffer<Bulk4> Resident : register(u0);
RWStructuredBuffer<Bulk4> Pending : register(u1);
RWStructuredBuffer<Bulk2> Capacity : register(u2);
RWStructuredBuffer<float> Aperture : register(u3);
RWStructuredBuffer<float4> Weights : register(u4); // area sum, admitted, routed, old excess
RWStructuredBuffer<Bulk4> Transfers : register(u5);
struct Metrics {float4 pending,activity,bounds;uint4 counts;};
RWStructuredBuffer<Metrics> Partials : register(u6);
RWStructuredBuffer<Metrics> Totals : register(u7);
RWByteAddressBuffer Control : register(u8); // routable donors, actual iterations, invalid
RWByteAddressBuffer Arguments : register(u9);
uint index(uint3 p){return (p.z*Grid.y+p.y)*Grid.x+p.x;}
uint3 coord(uint id){return uint3(id%Grid.x,(id/Grid.x)%Grid.y,id/(Grid.x*Grid.y));}
uint stride(){return (Grid.x+1)*(Grid.y+1)*(Grid.z+1);}
uint face(uint3 p,uint a){return a*stride()+(p.z*(Grid.y+1)+p.y)*(Grid.x+1)+p.x;}
bool inside(int3 p){return all(p>=0)&&all(p<int3(Grid.xyz));}
float edge(uint3 p,uint a,int direction){
    int3 other=int3(p);other[a]+=direction;
    if(!inside(other)||Capacity[index(uint3(other))].x<=0)return 0;
    uint3 f=p;if(direction>0)f[a]++;
    return Aperture[face(f,a)];
}
void accept(uint id,inout Bulk4 q){
    Bulk4 r=Resident[id];BulkScalar room=max(0,Capacity[id].x-r.w);
    BulkScalar amount=min(q.w,room);
    Bulk4 admitted=q.w>0?q*(amount/q.w):0;admitted.w=amount;
    r+=admitted;q-=admitted;Resident[id]=r;Weights[id].y+=float(amount);
    // Error-controlled scheduling only: sub-tolerance requests stay in Pending,
    // the mass/momentum totals, and are admitted if capacity later becomes free.
    if(q.w>RoutingTolerance&&Weights[id].x>0)Control.InterlockedAdd(0,1);
    if(any(!isfinite(float4(q)))||any(!isfinite(float4(r)))||q.w<0||r.w<0)Control.InterlockedAdd(8,1);
}
[numthreads(1,1,1)]void AllocationClear(){Control.Store(0,0);Control.Store(4,0);Control.Store(8,0);}
[numthreads(128,1,1)]void AllocationAccept(uint id:SV_DispatchThreadID){
    if(id>=Grid.w)return;uint3 p=coord(id);float sum=0;
    [unroll]for(uint a=0;a<3;a++)sum+=edge(p,a,-1)+edge(p,a,1);
    Weights[id]=float4(sum,0,0,max(0,Resident[id].w-Capacity[id].x));
    if(Advance){Bulk4 q=Pending[id];accept(id,q);Pending[id]=q;}
}
[numthreads(1,1,1)]void AllocationPrepare(){
    bool active=Control.Load(0)>0;
    Arguments.Store3(0,uint3(active?(3*stride()+127)/128:0,1,1));
    Arguments.Store3(12,uint3(active?(Grid.w+127)/128:0,1,1));
    if(active&&Iteration<64)Control.InterlockedAdd(4,1);
    Control.Store(0,0);
}
[numthreads(128,1,1)]void AllocationFlux(uint id:SV_DispatchThreadID){
    if(id>=3*stride())return;uint a=id/stride(),k=id%stride();
    uint3 p=uint3(k%(Grid.x+1),(k/(Grid.x+1))%(Grid.y+1),k/((Grid.x+1)*(Grid.y+1)));
    uint3 extent=Grid.xyz;extent[a]++;
    if(any(p>=extent)||p[a]==0||p[a]==Grid[a]){Transfers[id]=0;return;}
    uint3 l=p;l[a]--;uint left=index(l),right=index(p);
    // Damped, aperture-connected routing avoids a bipartite two-cycle. Each
    // donor sends at most half its pending inventory with the SAME coefficients
    // for volume and all momentum components. No resident water is routed.
    float area=Aperture[id];float wl=Weights[left].x,wr=Weights[right].x;
    Bulk4 fromLeft=wl>0&&Capacity[right].x>0?Pending[left]*(.5*area/wl):0;
    Bulk4 fromRight=wr>0&&Capacity[left].x>0?Pending[right]*(.5*area/wr):0;
    Transfers[id]=fromLeft-fromRight;
}
[numthreads(128,1,1)]void AllocationUpdate(uint id:SV_DispatchThreadID){
    if(id>=Grid.w)return;uint3 p=coord(id);Bulk4 q=Pending[id];
    if(Weights[id].x>0)Weights[id].z+=float(q.w*.5);
    [unroll]for(uint a=0;a<3;a++){uint3 r=p;r[a]++;q+=Transfers[face(p,a)]-Transfers[face(r,a)];}
    accept(id,q);Pending[id]=q;
}
groupshared Metrics Shared[128];
Metrics merge(Metrics a,Metrics b){a.pending+=b.pending;a.activity+=b.activity;
    a.bounds=max(a.bounds,b.bounds);a.counts+=b.counts;return a;}
void reduce(uint lane){GroupMemoryBarrierWithGroupSync();for(uint s=64;s>0;s>>=1){
    if(lane<s)Shared[lane]=merge(Shared[lane],Shared[lane+s]);GroupMemoryBarrierWithGroupSync();}}
[numthreads(128,1,1)]void AllocationReduce(uint id:SV_DispatchThreadID,uint group:SV_GroupID,uint lane:SV_GroupIndex){
    Metrics m=(Metrics)0;
    if(id<Grid.w){Bulk4 q=Pending[id];float4 w=Weights[id];BulkScalar excess=max(0,Resident[id].w-Capacity[id].x);
        m.pending=float4(q);m.activity=float4(w.y,w.z,0,0);
        m.bounds=float4(max(0,excess-w.w),q.w>0?length(float3(q.xyz/q.w)):0,0,0);
        m.counts.x=q.w>0?1:0;m.counts.y=q.w>0&&w.x==0?1:0;
        m.counts.z=any(!isfinite(float4(q)))||q.w<0?1:0;}
    Shared[lane]=m;reduce(lane);if(lane==0)Partials[group]=Shared[0];
}
[numthreads(128,1,1)]void AllocationTotals(uint lane:SV_GroupIndex){
    Metrics m=(Metrics)0;for(uint i=lane;i<(Grid.w+127)/128;i+=128)m=merge(m,Partials[i]);
    Shared[lane]=m;reduce(lane);if(lane==0){m=Shared[0];m.counts.z+=Control.Load(8);m.counts.w=Control.Load(4);Totals[0]=m;}
}
