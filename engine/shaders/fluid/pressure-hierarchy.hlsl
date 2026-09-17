// Galerkin aggregation: piecewise-constant prolongation P, R=P^T, Ac=P^T A P.
// All physical boundary/ghost terms remain in the original assembled operator.
cbuffer PressureFrame : register(b0){uint4 Grid,CoarseGrid,Tiles;float4 Settings;};
RWStructuredBuffer<float> FineIn : register(u0);
RWStructuredBuffer<float> FineOut : register(u1);
RWStructuredBuffer<float4> Stencil : register(u2); // rhs, inverse diagonal, neighbor mask
RWStructuredBuffer<uint> Work : register(u3); // fine partition, then coarse partition
RWByteAddressBuffer Counters : register(u4);
RWByteAddressBuffer Arguments : register(u5);
struct Coefficients {float4 negative,positive;}; // negative.w=diagonal, positive.w=child count
RWStructuredBuffer<Coefficients> CoarseA : register(u6);
RWStructuredBuffer<float> CoarseRhs : register(u7);
RWStructuredBuffer<float> CoarseIn : register(u8);
RWStructuredBuffer<float> CoarseOut : register(u9);
RWStructuredBuffer<uint> TileFlags : register(u10);
uint3 coord(uint id,uint3 n){return uint3(id%n.x,(id/n.x)%n.y,id/(n.x*n.y));}
uint index(uint3 p,uint3 n){return (p.z*n.y+p.y)*n.x+p.x;}
float neighbors(uint id){
    uint mask=uint(Stencil[id].z),strides[3]={1,Grid.x,Grid.x*Grid.y};float sum=0;
    [unroll]for(uint a=0;a<3;++a){
        if(mask&(1u<<(2*a)))sum+=FineIn[id-strides[a]];
        if(mask&(2u<<(2*a)))sum+=FineIn[id+strides[a]];
    }
    return sum;
}
float residualAt(uint id){float4 s=Stencil[id];return s.y>0?s.x+neighbors(id)-FineIn[id]/s.y:0;}
[numthreads(128,1,1)]
void PressureClear(uint id:SV_DispatchThreadID){if(id<8)Counters.Store(id*4,0);if(id<Tiles.w)TileFlags[id]=0;}
[numthreads(128,1,1)]
void PressureActive(uint id:SV_DispatchThreadID){
    if(id>=Grid.w||Stencil[id].y==0)return;
    uint slot;Counters.InterlockedAdd(0,1,slot);Work[slot]=id;
    Counters.InterlockedMax(12,asuint(abs(Stencil[id].x)));
    uint tile=index(coord(id,Grid.xyz)/uint3(8,4,4),Tiles.xyz),wasActive;
    InterlockedOr(TileFlags[tile],1,wasActive);
    if(!wasActive){Counters.InterlockedAdd(20,1,slot);Work[Grid.w+CoarseGrid.w+slot]=tile;}
}
[numthreads(128,1,1)]
void PressureAssemble(uint id:SV_DispatchThreadID){
    if(id>=CoarseGrid.w)return;
    uint3 base=coord(id,CoarseGrid.xyz)*2;Coefficients c=(Coefficients)0;
    [unroll]for(uint child=0;child<8;++child){
        uint3 p=base+uint3(child&1,(child>>1)&1,child>>2);if(any(p>=Grid.xyz))continue;
        uint fid=index(p,Grid.xyz);float4 s=Stencil[fid];if(s.y==0)continue;
        c.negative.w+=round(1/s.y);c.positive.w++;
        uint mask=uint(s.z);
        [unroll]for(uint a=0;a<3;++a)[unroll]for(uint side=0;side<2;++side){
            if(!(mask&(1u<<(2*a+side))))continue;
            // Mask already excludes air, solid and out-of-domain neighbors.
            int3 n=int3(p);n[a]+=side?1:-1;
            if(all(uint3(n)/2==p/2))c.negative.w-=1;
            else if(side)c.positive[a]++;else c.negative[a]++;
        }
    }
    CoarseA[id]=c;
    if(c.positive.w>0){uint slot;Counters.InterlockedAdd(4,1,slot);Work[Grid.w+slot]=id;}
}
[numthreads(1,1,1)]
void PressurePrepare(){
    Arguments.Store3(0,uint3((Counters.Load(0)+127)/128,1,1));
    Arguments.Store3(12,uint3((Counters.Load(4)+127)/128,1,1));
    Arguments.Store3(24,uint3(Counters.Load(20),1,1));
}
groupshared float FirstSweep[360]; // (8+2)*(4+2)*(4+2), one-step halo
[numthreads(128,1,1)]
void PressureTileSmooth(uint group:SV_GroupID,uint lane:SV_GroupIndex){
    uint tile=Work[Grid.w+CoarseGrid.w+group];int3 base=int3(coord(tile,Tiles.xyz)*uint3(8,4,4));
    // First global Jacobi update for tile AND halo: reads only the original
    // global iterate, never a neighboring group's writes.
    for(uint node=lane;node<360;node+=128){
        int3 p=base+int3(node%10,(node/10)%6,node/60)-1;float value=0;
        if(all(p>=0)&&all(p<int3(Grid.xyz))){
            uint id=index(p,Grid.xyz);float4 s=Stencil[id];
            if(s.y>0){float j=(neighbors(id)+s.x)*s.y;
                value=Settings.x==1?j:FineIn[id]+Settings.x*(j-FineIn[id]);}
        }
        FirstSweep[node]=value;
    }
    GroupMemoryBarrierWithGroupSync();
    uint3 local=uint3(lane%8,(lane/8)%4,lane/32);uint3 p=uint3(base)+local;
    if(any(p>=Grid.xyz))return;
    uint id=index(p,Grid.xyz);float4 s=Stencil[id];if(s.y==0)return;
    uint here=((local.z+1)*6+local.y+1)*10+local.x+1;
    uint stride[3]={1,10,60},mask=uint(s.z);float sum=0;
    [unroll]for(uint a=0;a<3;++a){
        if(mask&(1u<<(2*a)))sum+=FirstSweep[here-stride[a]];
        if(mask&(2u<<(2*a)))sum+=FirstSweep[here+stride[a]];
    }
    float jacobi=(sum+s.x)*s.y;
    float value=Settings.x==1?jacobi:FirstSweep[here]+Settings.x*(jacobi-FirstSweep[here]);
    if(!isfinite(value)){Counters.InterlockedAdd(8,1);value=0;}
    FineOut[id]=value;
}
[numthreads(128,1,1)]
void PressureSmooth(uint thread:SV_DispatchThreadID){
    if(thread>=Counters.Load(0))return;
    uint id=Work[thread];float4 s=Stencil[id];
    float jacobi=(neighbors(id)+s.x)*s.y;
    // Active mode is the exact original Jacobi update, without extra arithmetic.
    float value=Settings.x==1?jacobi:FineIn[id]+Settings.x*(jacobi-FineIn[id]);
    if(!isfinite(value)){Counters.InterlockedAdd(8,1);value=0;}
    FineOut[id]=value;
}
[numthreads(128,1,1)]
void PressureRestrict(uint thread:SV_DispatchThreadID){
    if(thread>=Counters.Load(4))return;
    uint id=Work[Grid.w+thread];uint3 base=coord(id,CoarseGrid.xyz)*2;float sum=0;
    [unroll]for(uint child=0;child<8;++child){
        uint3 p=base+uint3(child&1,(child>>1)&1,child>>2);
        if(all(p<Grid.xyz))sum+=residualAt(index(p,Grid.xyz));
    }
    // SUM, not average: restriction must be the transpose of constant P.
    CoarseRhs[id]=sum;CoarseIn[id]=0;CoarseOut[id]=0;
}
[numthreads(128,1,1)]
void PressureCoarseSmooth(uint thread:SV_DispatchThreadID){
    if(thread>=Counters.Load(4))return;
    uint id=Work[Grid.w+thread];Coefficients c=CoarseA[id];
    uint stride[3]={1,CoarseGrid.x,CoarseGrid.x*CoarseGrid.y};float sum=0;
    [unroll]for(uint a=0;a<3;++a){
        if(c.negative[a]>0)sum+=c.negative[a]*CoarseIn[id-stride[a]];
        if(c.positive[a]>0)sum+=c.positive[a]*CoarseIn[id+stride[a]];
    }
    // A fully enclosed single aggregate has a constant-pressure nullspace.
    float value=c.negative.w>0?CoarseIn[id]+(2.0/3.0)*((sum+CoarseRhs[id])/c.negative.w-CoarseIn[id]):0;
    if(!isfinite(value)){Counters.InterlockedAdd(8,1);value=0;}
    CoarseOut[id]=value;
}
[numthreads(128,1,1)]
void PressureProlongate(uint thread:SV_DispatchThreadID){
    if(thread>=Counters.Load(0))return;
    uint id=Work[thread],parent=index(coord(id,Grid.xyz)/2,CoarseGrid.xyz);
    FineIn[id]+=CoarseIn[parent];
}
[numthreads(128,1,1)]
void PressureResidual(uint thread:SV_DispatchThreadID){
    if(thread>=Counters.Load(0))return;
    float r=abs(residualAt(Work[thread]));
    if(!isfinite(r))Counters.InterlockedAdd(8,1);
    else Counters.InterlockedMax(16,asuint(r));
}
