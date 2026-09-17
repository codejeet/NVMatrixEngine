// Bulk-filled interiors supply missing pressure/transfer coverage. Partially
// filled interface cells stay particle-defined until geometric VOF is connected,
// except an opt-in disappearing mixture that requires a pressure escape path.
#include "bulk-numeric.hlsli"
cbuffer SupportFrame : register(b0){uint4 Fine,Coarse,Control;float4 Parameters;};
RWStructuredBuffer<Bulk4> Inventory : register(u0);
RWStructuredBuffer<float2> FineVolume : register(u1);
RWStructuredBuffer<Bulk2> CoarseVolume : register(u2);
RWStructuredBuffer<float4> Cells : register(u3);
RWStructuredBuffer<float4> Faces : register(u4);
RWStructuredBuffer<uint> Metrics : register(u5);
RWStructuredBuffer<float> Aperture : register(u6);
RWStructuredBuffer<float> FilledVolumeFraction : register(u7);
uint index(uint3 p,uint3 n){return (p.z*n.y+p.y)*n.x+p.x;}
uint3 coord(uint i,uint3 n){return uint3(i%n.x,(i/n.x)%n.y,i/(n.x*n.y));}
uint stride(){return (Fine.x+1)*(Fine.y+1)*(Fine.z+1);}
bool inside(int3 p){return all(p>=0)&&all(p<int3(Fine.xyz));}
float supportVolume(uint3 p){float2 v=FineVolume[index(p,Fine.xyz)];return Control.y?.5*(v.x+v.y):v.x;}
float supportFraction(uint3 p){uint c=index(p/2,Coarse.xyz);Bulk2 v=CoarseVolume[c];BulkScalar start=Control.x?v.y:v.x;
    if(start<=0)return 0;
    if(Inventory[c].w>=start*(1-Parameters.y))return 1;
    bool evacuate=((Control.z&1)&&v.x==0)||((Control.z&2)&&v.x<start&&Inventory[c].w>v.x);
    return Control.x&&evacuate&&Inventory[c].w>0?float(Inventory[c].w/start):0;
}
[numthreads(8,1,1)]void BulkPressureClear(uint id:SV_GroupIndex){Metrics[id]=0;}
[numthreads(128,1,1)]void BulkPressureCells(uint id:SV_DispatchThreadID){
    if(id>=Fine.w)return;uint3 p=coord(id,Fine.xyz);float4 c=Cells[id];FilledVolumeFraction[id]=0;
    float fraction=supportFraction(p);
    if(c.z==2||supportVolume(p)<=0||fraction<=0)return;
    FilledVolumeFraction[id]=supportVolume(p)*fraction/Parameters.z;
    if(fraction<1){InterlockedAdd(Metrics[4],1);if(FineVolume[id].x>0)InterlockedAdd(Metrics[5],1);}
    InterlockedAdd(Metrics[2],1);
    if(c.z==0){c.z=1;Cells[id]=c;InterlockedAdd(Metrics[0],1);
        if(FineVolume[id].x==0)InterlockedAdd(Metrics[3],1);}
}
// Existing particle/owner P2G faces remain bitwise unchanged. Only missing
// faces receive an Eulerian velocity, with the same pre-force FLIP reference.
[numthreads(128,1,1)]void BulkPressureFaces(uint id:SV_DispatchThreadID){
    if(id>=3*stride())return;uint axis=id/stride();uint3 p=coord(id%stride(),Fine.xyz+1),extent=Fine.xyz;extent[axis]++;
    if(any(p>=extent)||Faces[id].z>1e-8)return;
    int3 left=int3(p);left[axis]--;
    if(!inside(left)||!inside(p)||Aperture[id]<=0||supportVolume(left)<=0||supportVolume(p)<=0)return;
    float momentum=0,volume=0;
    [unroll]for(uint side=0;side<2;++side){uint3 cell=side?p:uint3(left);
        float fraction=supportFraction(cell);
        if(Cells[index(cell,Fine.xyz)].z!=1||fraction<=0)continue;
        Bulk4 q=Inventory[index(cell/2,Coarse.xyz)];float w=.5*supportVolume(cell)*fraction;
        momentum+=float(w*q[axis]/q.w);volume+=w;
    }
    if(volume<=0)return;
    float v=momentum/volume;float4 f=float4(v,v,volume/Parameters.x,0);
    if(any(!isfinite(f))){InterlockedAdd(Metrics[7],1);return;}
    Faces[id]=f;InterlockedAdd(Metrics[1],1);
}
