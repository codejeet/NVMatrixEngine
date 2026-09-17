// Two nested sampling lattices. The fine lattice remains the canonical DXR
// cache; coarse bricks reconstruct only 5^3 anchors and prolongate the rest.
struct SurfaceLodState {float4 error;float4 transition;uint4 state;};
// error: phi/h, normal chord, material displacement/h, coarse blend.
// transition: previous blend, accumulated displacement/h, tolerance scale, hard protection.
// state: occupancy signature, last full reconstruction, gather-fine, valid + unsafe face bits.
RWStructuredBuffer<SurfaceLodState> SurfaceLod : register(u13);
RWStructuredBuffer<SurfaceLodState> SurfaceLodNext : register(u14);
struct SurfaceImportance {float4 importance,dynamics,velocity;uint4 state;};
RWStructuredBuffer<SurfaceImportance> SurfaceComplexity : register(u15);
RWStructuredBuffer<uint> SurfaceLodLists : register(u16);
RWStructuredBuffer<float4> SurfaceReference : register(u17);
uint surfaceNodeIndex(uint3 p){return (p.z*9+p.y)*9+p.x;}
// One immutable axis mask per field keeps every shared face on the same
// nested lattice. Error/importance still choose fine or coarse per brick.
uint3 surfaceCoarseStride(){return 1+uint3(Collision.w&1,(Collision.w>>1)&1,(Collision.w>>2)&1);}
float4 surfaceCoarse(uint slot,uint3 p){
    uint3 step=surfaceCoarseStride(),a=min((p/step)*step,8-step);float3 f=float3(p-a)/float3(step);
    float4 v[8];[unroll]for(uint k=0;k<8;++k){uint3 n=a+step*uint3(k&1,(k>>1)&1,k>>2);v[k]=Field[slot*729+surfaceNodeIndex(n)];}
    return lerp(lerp(lerp(v[0],v[1],f.x),lerp(v[2],v[3],f.x),f.y),
                lerp(lerp(v[4],v[5],f.x),lerp(v[6],v[7],f.x),f.y),f.z);
}
float3 surfaceGradient(uint slot,uint3 p,bool coarse){
    float3 g=0;
    [unroll]for(uint a=0;a<3;++a){uint3 lo=p,hi=p;lo[a]=max(int(p[a])-1,0);hi[a]=min(p[a]+1,8u);
        float low=coarse?surfaceCoarse(slot,lo).x:Field[slot*729+surfaceNodeIndex(lo)].x;
        float high=coarse?surfaceCoarse(slot,hi).x:Field[slot*729+surfaceNodeIndex(hi)].x;
        g[a]=(high-low)/float(hi[a]-lo[a]);}
    return g;
}
float surfaceGlobalCoarse(int3 node){
    if(any(node<0)||any(node>=int3(Bricks.xyz*8)))return FieldMinimumSpacing.w*3;
    uint3 b=uint3(node)/8;uint slot=BrickMap[(b.z*Bricks.y+b.y)*Bricks.x+b.x];
    return slot==0xffffffff?FieldMinimumSpacing.w*3:surfaceCoarse(slot,uint3(node)%8).x;
}
float3 surfaceMotionGradient(uint brick,uint3 local){
    int3 node=int3(brickCoord(brick)*8+local);float3 g=0;
    [unroll]for(uint axis=0;axis<3;++axis){int3 e=0;e[axis]=1;
        g[axis]=(surfaceGlobalCoarse(node+e)-surfaceGlobalCoarse(node-e))/(2*FieldMinimumSpacing.w);}
    return g;
}
// Every brick sharing a node computes the SAME max blend. This is a hanging-
// node constraint on a shared coarse face/edge, not independently faded bricks.
float surfaceNodeBlend(uint brick,uint3 p,bool previous){
    int3 b=int3(brickCoord(brick));int3 lo=0,hi=0;
    [unroll]for(uint a=0;a<3;++a){if(p[a]==0)lo[a]=-1;if(p[a]==8)hi[a]=1;}
    float blend=0;
    for(int z=lo.z;z<=hi.z;++z)for(int y=lo.y;y<=hi.y;++y)for(int x=lo.x;x<=hi.x;++x){
        int3 q=b+int3(x,y,z);if(any(q<0)||any(q>=int3(Bricks.xyz)))continue;
        uint id=(q.z*Bricks.y+q.y)*Bricks.x+q.x;
        SurfaceLodState s;
        if(previous)s=SurfaceLod[id];else s=SurfaceLodNext[id];
        if(s.state.w)blend=max(blend,previous?s.error.w:asfloat(SurfaceLodLists[2*Bricks.w+id]));
    }
    return blend;
}
#include "surface-band.hlsli"
[numthreads(128,1,1)]void SurfaceLodBegin(uint id:SV_DispatchThreadID){
    if(id<20)Counters.Store(16+4*id,0);
    if(id<Bricks.w&&LodControl.z){SurfaceLodState s=(SurfaceLodState)0;s.error.xy=1e6;SurfaceLod[id]=s;}
}
uint surfaceHash(uint x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);}
uint surfaceHash4(float4 v){uint4 q=asuint(v);return surfaceHash(q.x)^surfaceHash(q.y+0x9e3779b9u)^surfaceHash(q.z+0x85ebca6bu)^surfaceHash(q.w+0xc2b2ae35u);}
[numthreads(128,1,1)]void SurfaceLodFingerprint(uint cell:SV_DispatchThreadID){
    if(cell>=SimulationGrid.w)return;
    uint count=Offsets[cell+1]-Offsets[cell];
    for(uint entry=Offsets[cell];entry<Offsets[cell+1];++entry){uint particle=Indices[entry];
        Particle a=Particles[particle];ParticleShape shape=Shapes[particle];
        uint key=surfaceHash4(float4(a.positionRadius.xyz,a.apic0.w));
        key=surfaceHash(key^surfaceHash4(shape.row0));key=surfaceHash(key^surfaceHash4(shape.row1));
        key=surfaceHash(key^surfaceHash4(shape.row2));key=surfaceHash(key^surfaceHash4(float4(Previous[particle].xyz,0)));
        count+=surfaceHash(key^particle);
    }
    if(Collision.z){uint3 p=uint3(cell%SimulationGrid.x,(cell/SimulationGrid.x)%SimulationGrid.y,cell/(SimulationGrid.x*SimulationGrid.y));
        InteriorCell c=Interior[interiorIndex(p/2,SimulationGrid.xyz)];
        count+=interiorCellQuanta(p,SimulationGrid.xyz,SimulationMinimumCell);
        if(c.velocityFlags.w)count+=surfaceHash4(c.positionRadius)^surfaceHash4(float4(c.velocityFlags.w,c.apic1.w,c.apic2.w,0));
    }
#if FLUID_NARROW_SURFACE
    uint3 nc=uint3(cell%SimulationGrid.x,(cell/SimulationGrid.x)%SimulationGrid.y,cell/(SimulationGrid.x*SimulationGrid.y));
    double4 nq=NarrowSurfaceGrid[narrowSurfaceIndex(int3(nc/2))];
    count+=surfaceHash4(float4(nq));
    count+=surfaceHash(asuint(PhaseControl.x));
#endif
    SurfaceLodLists[3*Bricks.w+cell]=count;
}
[numthreads(128,1,1)]void SurfaceLodPlan(uint id:SV_DispatchThreadID){
    if(id>=Bricks.w)return;SurfaceLodState next=(SurfaceLodState)0;
    uint slot=BrickMap[id];if(slot==0xffffffff){SurfaceLodNext[id]=next;return;}
    SurfaceLodState old=SurfaceLod[id];
    // FieldMinimumSpacing starts two MAC cells before SimulationMinimumCell.
    // The -2 is the lattice-origin offset, not the gather halo below.
    int3 base=int3(brickCoord(id)*4)-2;uint signature=2166136261u;
    // Full 1.8-MAC-cell reconstruction halo. Hash actual weighted positions,
    // current covariance kernels and material guides, not just bin occupancy.
    // Shape changes caused by neighbors outside the gather halo are included
    // through the freshly computed Shapes. Integer sums are order independent.
    if(Reconstruction.w==0)for(int z=-2;z<=5;++z)for(int y=-2;y<=5;++y)for(int x=-2;x<=5;++x){
        int3 p=base+int3(x,y,z);uint count=0;
        if(all(p>=0)&&all(p<int3(SimulationGrid.xyz)))count=SurfaceLodLists[3*Bricks.w+simIndex(p)];
        signature=(signature^count)*16777619u;
    }
    float3 center=FieldMinimumSpacing.xyz+(float3(brickCoord(id)*8)+4)*FieldMinimumSpacing.w;
    float radius=length(float3(4,4,4))*FieldMinimumSpacing.w;
    bool protect=(LodControl.w&8u)!=0||Reconstruction.w==1||Reconstruction.w==2;
    float scale=1,displacement=old.error.z;
    if(LodControl.w&1u){SurfaceImportance importance=SurfaceComplexity[id];
        scale=lerp(1,.5,saturate(max(importance.importance.z,importance.importance.w)));
        if(LodControl.w&4u){
            protect|=importance.dynamics.x>.05||importance.dynamics.y>.2||importance.dynamics.z>.2;
            protect|=(importance.state.z&4u)!=0;
        }
    }
    if(!(LodControl.w&4u))displacement=0;
    protect|=length(center-LodCamera.xyz)-radius<1.5;
    for(uint i=0;i<Collision.x;++i){FluidCollider c=Colliders[i];
        // Imported mesh phi is immutable; transforms, primitive shape and mesh
        // asset addressing all affect carving, including paused teleports.
        if(colliderPhi(c,center)<radius+2*FieldMinimumSpacing.w){
            [unroll]for(uint row=0;row<4;++row)signature=surfaceHash(signature^surfaceHash4(c.worldToLocal[row]));
            signature=surfaceHash(signature^surfaceHash4(c.extentType)^surfaceHash4(c.meshMinimumSpacing)^surfaceHash4(asfloat(c.meshDimensions)));
        }
        if(length(colliderVelocity(c,center))+length(c.angularSlip.xyz)*radius>.001&&
           colliderPhi(c,center)<radius+2*FieldMinimumSpacing.w)protect=true;
    }
    float drift=old.transition.y+displacement;
    bool accurate=old.error.x<LodTolerance.x*scale*.75&&old.error.y<LodTolerance.y*scale*.75;
    bool eligible=old.state.w&&accurate&&!protect&&drift<LodTolerance.w;
    float blend=eligible?min(1,old.error.w+LodTolerance.z):0;
    bool full=old.state.x!=signature||blend<1||blend!=old.error.w||LodControl.y-old.state.y>=uint(LodCamera.w);
    next.error=float4(old.error.xyz,blend);
    next.transition=float4(old.error.w,drift,scale,protect?1:0);
    next.state=uint4(signature,old.state.y,full?1:0,old.state.w|1u);
    SurfaceLodNext[id]=next;uint ticket;
    Counters.InterlockedAdd(full?16:20,1,ticket);
    SurfaceLodLists[(full?0:Bricks.w)+ticket]=slot;
}
[numthreads(128,1,1)]void SurfaceLodFine(uint group:SV_GroupID,uint lane:SV_GroupIndex){
    uint slot=SurfaceLodLists[group/6],node=(group%6)*128+lane;if(node>=729)return;
    Field[slot*729+node]=reconstructNode(BrickList[slot],uint3(node%9,(node/9)%9,node/81));
    if(lane==0)Counters.InterlockedAdd(32,min(128u,729-(group%6)*128));
}
[numthreads(128,1,1)]void SurfaceLodCoarse(uint group:SV_GroupID,uint lane:SV_GroupIndex){
    uint slot=SurfaceLodLists[Bricks.w+group];
    uint3 step=surfaceCoarseStride(),dims=8/step+1;uint nodes=dims.x*dims.y*dims.z;
    for(uint n=lane;n<nodes;n+=128){uint3 p=step*uint3(n%dims.x,(n/dims.x)%dims.y,n/(dims.x*dims.y));
        Field[slot*729+surfaceNodeIndex(p)]=reconstructNode(BrickList[slot],p);}
    if(lane==0)Counters.InterlockedAdd(36,nodes);
}
groupshared float3 SurfaceLodErrors[128];
groupshared uint SurfaceLodUnsafeFaces[128];
groupshared uint SurfaceLodBandCounts[128];
[numthreads(128,1,1)]void SurfaceLodAnalyze(uint slot:SV_GroupID,uint lane:SV_GroupIndex){
    uint id=BrickList[slot];SurfaceLodState s=SurfaceLodNext[id];if(!s.state.z)return;
    surfaceLoadBand(id,lane);
    float3 error=0;float h=FieldMinimumSpacing.w;uint unsafeFaces=0,bandNodes=0;
    for(uint n=lane;n<729;n+=128){uint3 p=uint3(n%9,(n/9)%9,n/81);
        float4 fine=Field[slot*729+n],coarse=surfaceCoarse(slot,p);
        error.z=max(error.z,length(fine.yzw)/h);
        float2 nodeError=0;
        // Actual zero-crossing cells plus their optical normal stencils.
        if(surfaceInBand(p)){
            ++bandNodes;
            nodeError.x=abs(fine.x-coarse.x)/h;
            float3 a=surfaceGradient(slot,p,false),b=surfaceGradient(slot,p,true);
            if(dot(a,a)>1e-12&&dot(b,b)>1e-12)nodeError.y=length(normalize(a)-normalize(b));
            else if(abs(fine.x)<h*.1)nodeError.y=1e6;
        }
        // Reject a lost fine-lattice sign event, including small disconnected
        // sheets/droplets invisible to the coarse anchors.
        if((fine.x<0)!=(coarse.x<0)&&max(abs(fine.x),abs(coarse.x))>h*1e-6)nodeError=1e6;
        error.xy=max(error.xy,nodeError);
        if(any(nodeError>float2(LodTolerance.xy)*s.transition.z*.5)){
            [unroll]for(uint axis=0;axis<3;++axis){
                if(p[axis]<=1)unsafeFaces|=1u<<(2*axis);
                if(p[axis]>=7)unsafeFaces|=1u<<(2*axis+1);
            }
        }
    }
    SurfaceLodErrors[lane]=error;SurfaceLodUnsafeFaces[lane]=unsafeFaces;SurfaceLodBandCounts[lane]=bandNodes;GroupMemoryBarrierWithGroupSync();
    for(uint stride=64;stride;stride/=2){if(lane<stride){SurfaceLodErrors[lane]=max(SurfaceLodErrors[lane],SurfaceLodErrors[lane+stride]);SurfaceLodUnsafeFaces[lane]|=SurfaceLodUnsafeFaces[lane+stride];SurfaceLodBandCounts[lane]+=SurfaceLodBandCounts[lane+stride];}GroupMemoryBarrierWithGroupSync();}
    if(lane)return;
    Counters.InterlockedAdd(84,SurfaceLodBandCounts[0]);
    s.error.xyz=SurfaceLodErrors[0];s.state.y=LodControl.y;s.state.w=1u|(SurfaceLodUnsafeFaces[0]<<1);s.transition.y=0;
    if(s.error.x>LodTolerance.x*s.transition.z||s.error.y>LodTolerance.y*s.transition.z)s.error.w=0;
    SurfaceLodNext[id]=s;Counters.InterlockedAdd(40,1);
}
[numthreads(128,1,1)]void SurfaceLodGuard(uint id:SV_DispatchThreadID){
    if(id>=Bricks.w)return;
    SurfaceLodState s=SurfaceLodNext[id];float blend=s.error.w;
    // A shared hanging node affects the neighboring brick's normal stencil.
    // Pad unsafe shared faces (including their normal stencil), not unrelated
    // detail in a neighboring brick's interior. Analysis is immutable here.
    int3 b=int3(brickCoord(id));
    for(int z=-1;z<=1&&blend>0;++z)for(int y=-1;y<=1&&blend>0;++y)for(int x=-1;x<=1&&blend>0;++x){
        int3 p=b+int3(x,y,z);if(any(p<0)||any(p>=int3(Bricks.xyz)))continue;
        uint other=(p.z*Bricks.y+p.y)*Bricks.x+p.x;SurfaceLodState q=SurfaceLodNext[other];
        if(q.state.w){
            if(q.transition.w!=0)blend=0;
            int3 offset=int3(x,y,z);
            [unroll]for(uint axis=0;axis<3;++axis)if(offset[axis]!=0){
                uint face=2*axis+(offset[axis]<0?1u:0u);
                if(q.state.w&(2u<<face))blend=0;
            }
        }
    }
    SurfaceLodLists[2*Bricks.w+id]=asuint(blend);
}
[numthreads(128,1,1)]void SurfaceLodRepair(uint group:SV_GroupID,uint lane:SV_GroupIndex){
    uint slot=group/6,n=(group%6)*128+lane;if(n>=729)return;
    uint id=BrickList[slot];
    if(SurfaceLodNext[id].state.z||asfloat(SurfaceLodLists[2*Bricks.w+id])==1)return;
    // A freshly failed neighbor can revoke a coarse gather after scheduling.
    // Restore its fine samples before finalization; never expose stale odd nodes.
    Field[slot*729+n]=reconstructNode(id,uint3(n%9,(n/9)%9,n/81));
    if(lane==0){Counters.InterlockedAdd(52,min(128u,729-(group%6)*128));if(group%6==0)Counters.InterlockedAdd(48,1);}
}
[numthreads(128,1,1)]void SurfaceLodFinalize(uint group:SV_GroupID,uint lane:SV_GroupIndex){
    uint slot=group/6,n=(group%6)*128+lane;if(n>=729)return;
    uint id=BrickList[slot];uint3 p=uint3(n%9,(n/9)%9,n/81);
    // Anchors are immutable for this dispatch; all readers see identical data.
    if(all((p%surfaceCoarseStride())==0))return;
    float blend=surfaceNodeBlend(id,p,false),oldBlend=surfaceNodeBlend(id,p,true);
    if(blend==0&&oldBlend==0)return;
    float4 coarse=surfaceCoarse(slot,p),fine=coarse;
    if(SurfaceLodNext[id].state.z||asfloat(SurfaceLodLists[2*Bricks.w+id])!=1)fine=Field[slot*729+n];
    float4 value=lerp(fine,coarse,blend);
    if(blend!=oldBlend&&surfaceInBandGlobal(id,p)){float3 g=surfaceMotionGradient(id,p);
        float displacement=(blend-oldBlend)*(coarse.x-fine.x);
        value.yzw+=g*(displacement/max(dot(g,g),1e-8));
        if(displacement!=0){InterlockedOr(BrickMap[Bricks.w*18],1);Counters.InterlockedOr(76,1);}}
    Field[slot*729+n]=value;
}
[numthreads(128,1,1)]void SurfaceLodCommit(uint id:SV_DispatchThreadID){
    if(id>=Bricks.w)return;SurfaceLodState s=SurfaceLodNext[id];
    float approved=asfloat(SurfaceLodLists[2*Bricks.w+id]);
    if(!s.state.z&&approved!=1){s.error.xy=1e6;s.state.z=1;}
    bool veto=approved!=s.error.w;s.error.w=approved;
    SurfaceLodNext[id]=s;SurfaceLod[id]=s;
    if(!s.state.w)return;
    bool good=s.error.x<LodTolerance.x*s.transition.z*.75&&s.error.y<LodTolerance.y*s.transition.z*.75;
    if(good&&!veto&&s.transition.w==0&&s.error.w<1)Counters.InterlockedOr(44,1);
    if(s.error.w==1)Counters.InterlockedAdd(72,1);
}
[numthreads(128,1,1)]void SurfaceLodReference(uint group:SV_GroupID,uint lane:SV_GroupIndex){
    uint slot=group/6,n=(group%6)*128+lane;if(n>=729)return;
    SurfaceReference[slot*729+n]=reconstructNode(BrickList[slot],uint3(n%9,(n/9)%9,n/81));
}
