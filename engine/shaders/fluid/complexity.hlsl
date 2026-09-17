// Observer of the current solver. Requested LODs do not change liquid mass or transport.
cbuffer ComplexityFrame : register(b0) {
    row_major float4x4 ViewProjection;
    float4 MinimumSpacing, SimulationMinimumCell;
    uint4 Bricks, Grid;
    float4 CameraPosition, CameraForward, Timing, Scales, Promote, Demote, Wake;
    uint4 Control;
};
#define FLUID_COLLIDER_REGISTER t1
#define FLUID_MESH_REGISTER t3
#include "solid.hlsli"
#include "complexity-policy.hlsli"
struct Brick {float4 importance,dynamics,velocity;uint4 state;};
RWStructuredBuffer<float4> Faces : register(u0);
RWStructuredBuffer<uint> Offsets : register(u1);
RWStructuredBuffer<float4> Material : register(u2);
RWStructuredBuffer<Brick> Raw : register(u3);
RWStructuredBuffer<Brick> State : register(u4);
RWStructuredBuffer<uint> Lists : register(u5);
RWStructuredBuffer<uint> Active : register(u6);
RWByteAddressBuffer Counts : register(u7);
RWByteAddressBuffer Arguments : register(u8);
RWStructuredBuffer<uint> CellQuanta : register(u9);
RWStructuredBuffer<uint2> OpticalFeedback : register(u10);
StructuredBuffer<uint> SurfaceMap : register(t0);
// flags: occupied, canonical surface, moving solid, padded refinement, active page
uint3 brickCoord(uint id){return uint3(id%Bricks.x,(id/Bricks.x)%Bricks.y,id/(Bricks.x*Bricks.y));}
uint brickIndex(uint3 p){return (p.z*Bricks.y+p.y)*Bricks.x+p.x;}
uint cellIndex(int3 p){return (p.z*Grid.y+p.y)*Grid.x+p.x;}
bool inside(int3 p){return all(p>=0)&&all(p<int3(Grid.xyz));}
uint population(int3 p){if(!inside(p))return 0;uint i=cellIndex(p);return Offsets[i+1]-Offsets[i];}
bool wet(int3 p){return inside(p)&&(population(p)>0||((Control.w&2u)&&CellQuanta[cellIndex(p)]>0));}
float3 velocity(int3 p){
    uint3 s=Grid.xyz+1;uint stride=s.x*s.y*s.z;
    uint i=(p.z*s.y+p.y)*s.x+p.x;
    return .5*float3(Faces[i].x+Faces[i+1].x,Faces[stride+i].x+Faces[stride+i+s.x].x,
                      Faces[2*stride+i].x+Faces[2*stride+i+s.x*s.y].x);
}
float normMetric(float x,float scale){return x/(scale+x);}
uint chooseLod(uint old,float value){
    return complexityLod(old,value,Promote.x,Promote.y,Promote.z,Demote.x,Demote.y,Demote.z);
}
float3 center(uint id){return MinimumSpacing.xyz+(float3(brickCoord(id))+.5)*8*MinimumSpacing.w;}
// Conservative sphere/swept capsule: angular motion counts even when linear v is zero.
float movingSolid(float3 p){
    float result=0,brickRadius=length(float3(4,4,4)*MinimumSpacing.w);
    for(uint i=0;i<Control.x;++i){
        FluidCollider c=Colliders[i];uint type=uint(c.extentType.w);
        float radius=length(c.extentType.xyz);float3 objectCenter=c.centerRestitution.xyz;
        if(type==0)radius=c.extentType.x;
        if(type==2)radius=c.extentType.x+c.extentType.y;
        if(type==5){float3 a=c.meshMinimumSpacing.xyz,b=a+float3(c.meshDimensions.xyz-1)*c.meshMinimumSpacing.w;
            // Imported meshes need not be centered at the transform origin.
            // Use their actual compact SDF bounds, not a huge origin-centered sphere.
            radius=.5*length(b-a);
            objectCenter=mul(.5*(a+b)-c.worldToLocal[3].xyz,transpose((float3x3)c.worldToLocal));}
        float3 v=colliderVelocity(c,objectCenter);
        float speed=length(v)+length(c.angularSlip.xyz)*radius;
        if(speed<.01)continue;
        float3 a=objectCenter-v*Wake.x,b=objectCenter+v*Wake.y;
        float3 ab=b-a;float t=saturate(dot(p-a,ab)/max(dot(ab,ab),1e-8));
        float distance=length(p-lerp(a,b,t))-radius-brickRadius;
        if(type==4)distance=abs(mul(float4(p,1),c.worldToLocal).y)-brickRadius;
        result=max(result,1-smoothstep(Scales.w,Scales.w+8*MinimumSpacing.w,distance));
    }
    return result;
}
[numthreads(64,1,1)]
void ComplexityClear(uint id:SV_DispatchThreadID){if(id<24)Counts.Store(id*4,0);}
groupshared float4 ReductionVelocity[64],ReductionMetrics[64];
groupshared uint ReductionParticles[64];
[numthreads(64,1,1)]
void ComplexityClassify(uint id:SV_GroupID,uint lane:SV_GroupIndex){
    // Lattice contract: render brick = 4 MAC cells, padded by 2 MAC cells.
    int3 cell=int3(brickCoord(id)*4)-2+int3(lane%4,(lane/4)%4,lane/16);
    uint count=population(cell);float3 v=0;float4 metric=0;
    if(wet(cell)){
        v=velocity(cell);float3 deriv[3];
        [unroll]for(uint axis=0;axis<3;++axis){
            int3 a=cell,b=cell;a[axis]--;b[axis]++;
            bool va=wet(a),vb=wet(b);
            float3 lo=va?velocity(a):v,hi=vb?velocity(b):v;
            deriv[axis]=(hi-lo)/(SimulationMinimumCell.w*max(uint(va)+uint(vb),1u));
        }
        float3 curl=float3(deriv[1].z-deriv[2].y,deriv[2].x-deriv[0].z,deriv[0].y-deriv[1].x);
        metric=float4(length(curl),sqrt(dot(deriv[0],deriv[0])+dot(deriv[1],deriv[1])+dot(deriv[2],deriv[2])),
                      Control.z!=0?abs(Material[cellIndex(cell)].y)*SimulationMinimumCell.w:0,0);
    }
    float mass=float(count);
    if(inside(cell)){
        if(Control.w&8u)mass=asfloat(CellQuanta[cellIndex(cell)]);
        else if(Control.w&1u)mass=CellQuanta[cellIndex(cell)]*(1.0/16);
    }
    ReductionVelocity[lane]=float4(v*mass,mass);ReductionMetrics[lane]=metric;ReductionParticles[lane]=count;
    GroupMemoryBarrierWithGroupSync();
    for(uint stride=32;stride>0;stride/=2){
        if(lane<stride){ReductionVelocity[lane]+=ReductionVelocity[lane+stride];
            ReductionMetrics[lane]=max(ReductionMetrics[lane],ReductionMetrics[lane+stride]);
            ReductionParticles[lane]+=ReductionParticles[lane+stride];}
        GroupMemoryBarrierWithGroupSync();
    }
    if(lane)return;
    Brick old=(Brick)0;if(Timing.z!=0&&Timing.w==0)old=State[id];else old.state.xy=3;
    Brick r=(Brick)0;uint particles=ReductionParticles[0];
    uint slot=SurfaceMap[id],mask=0;
    if(slot!=0xffffffff){[unroll]for(uint m=0;m<16;++m)mask|=SurfaceMap[Bricks.w+slot*17+m];}
    bool surface=mask!=0;float3 p=center(id);
    // Dry moving props should not allocate liquid refinement work. Retain a
    // one-brick prediction halo around the canonical particle-supported pages.
    bool nearLiquid=slot!=0xffffffff;int3 bc=int3(brickCoord(id));
    [loop]for(int z=-1;z<=1&&!nearLiquid;++z)[loop]for(int y=-1;y<=1&&!nearLiquid;++y)[loop]for(int x=-1;x<=1;++x){
        int3 q=bc+int3(x,y,z);if(all(q>=0)&&all(q<int3(Bricks.xyz)))nearLiquid|=SurfaceMap[brickIndex(q)]!=0xffffffff;}
    float solid=nearLiquid?movingSolid(p):0;
    r.state.z=(ReductionVelocity[0].w>0?1u:0u)|(surface?2u:0u)|(solid>.5?4u:0u)|(slot!=0xffffffff?16u:0u);
    r.state.w=particles;r.velocity=float4(ReductionVelocity[0].xyz/max(1,ReductionVelocity[0].w),ReductionVelocity[0].w);
    bool history=Timing.z!=0&&Timing.w==0;
    float accel=history&&Timing.x>0?length(r.velocity.xyz-old.velocity.xyz)/Timing.x:0;
    // A sample-count change is not a physical topology change. Observe rest mass.
    float change=history?saturate(abs(r.velocity.w-old.velocity.w)/max(r.velocity.w+old.velocity.w,1)):0;
    if(history&&((r.state.z^old.state.z)&3u))change=1;
    float4 metrics=ReductionMetrics[0];
    float dynamics=max(normMetric(metrics.x,Scales.x),normMetric(metrics.y,Scales.y));
    float temporal=max(change,normMetric(accel,Scales.z));
    float physics=max(surface?1:0,max(solid,max(dynamics,max(temporal*.8,saturate(metrics.z)*.7))));
    // Conservative projected bounding sphere. Visibility can raise optics, never lower physics.
    float4 clip=mul(float4(p,1),ViewProjection);
    float radius=length(float3(4,4,4)*MinimumSpacing.w);
    float3 cx=float3(ViewProjection[0][0],ViewProjection[1][0],ViewProjection[2][0]);
    float3 cy=float3(ViewProjection[0][1],ViewProjection[1][1],ViewProjection[2][1]);
    float3 cw=float3(ViewProjection[0][3],ViewProjection[1][3],ViewProjection[2][3]);
    float visible=clip.w+radius*length(cw)>0 &&
        clip.w+clip.x>=-radius*length(cw+cx)&&clip.w-clip.x>=-radius*length(cw-cx)&&
        clip.w+clip.y>=-radius*length(cw+cy)&&clip.w-clip.y>=-radius*length(cw-cy)?1:0;
    float optical=surface?max(.6,max(temporal,saturate(metrics.z))):0;
    float surf=surface?max(physics,visible*.9):0;
    // Previous rendered frame, consumed before the renderer clears this field.
    // Optical error can refine geometry but never lowers or changes bulk physics.
    if((Control.w&4u)&&surface){float2 feedback=asfloat(OpticalFeedback[id]);
        if(any(feedback>0)){Counts.InterlockedAdd(76,1);r.state.z|=32u;}
        if(max(feedback.x,feedback.y)>optical){Counts.InterlockedAdd(80,1);r.state.z|=64u;}
        optical=max(optical,max(feedback.x,feedback.y));surf=max(surf,optical);}
    float4 target=float4(physics,surf,optical,visible);
    float blend=history?1-exp(-max(Timing.x,0)/Timing.y):1;
    r.importance=lerp(old.importance,target,blend);
    // Immediate hard triggers; gradual decay protects wakes and topology transitions.
    r.importance.x=max(r.importance.x,max(surface?1:0,solid));
    r.importance.y=max(r.importance.y,surface?max(visible*.9,solid):0);
    r.dynamics=float4(temporal,metrics.x,metrics.y,accel);
    r.state.x=chooseLod(old.state.x,r.importance.x);r.state.y=chooseLod(old.state.y,r.importance.y);
    if(!all(isfinite(r.importance))||!all(isfinite(r.dynamics))||!all(isfinite(r.velocity))){
        Counts.InterlockedAdd(64,1);r.importance=1;r.dynamics=0;r.velocity=0;r.state.xy=0;}
    Raw[id]=r;
}
[numthreads(64,1,1)]
void ComplexitySchedule(uint id:SV_DispatchThreadID){
    if(id>=Bricks.w)return;
    Brick r=Raw[id];int3 coord=int3(brickCoord(id));uint sim=r.state.x,surf=r.state.y;
    // Read immutable raw decisions; no same-dispatch state races or directional padding bias.
    [loop]for(int z=-1;z<=1;++z)[loop]for(int y=-1;y<=1;++y)[loop]for(int x=-1;x<=1;++x){
        int3 q=coord+int3(x,y,z);if(any(q<0)||any(q>=int3(Bricks.xyz)))continue;
        Brick n=Raw[brickIndex(q)];
        if(n.state.x==0&&(n.state.z&7u))sim=0;
        if(n.state.y==0&&(n.state.z&2u))surf=0;
    }
    if(sim<r.state.x||surf<r.state.y)r.state.z|=8u;
    r.state.xy=uint2(sim,surf);
    if(Timing.z!=0&&Timing.w==0){uint old=State[id].state.x;
        if(sim<old)Counts.InterlockedAdd(68,1);if(sim>old)Counts.InterlockedAdd(72,1);}
    State[id]=r;
    // Active union includes empty padding/wake bricks, preparing space for disturbance propagation.
    if(!(r.state.z&31u))return;
    uint index;Counts.InterlockedAdd(0,1,index);Active[index]=id;
    Counts.InterlockedAdd((1+sim)*4,1,index);Lists[sim*Bricks.w+index]=id;
    Counts.InterlockedAdd((5+surf)*4,1,index);Lists[(4+surf)*Bricks.w+index]=id;
    Counts.InterlockedAdd((9+sim)*4,r.state.w);
    if(r.state.z&2u)Counts.InterlockedAdd(52,1);
    if(r.state.z&4u)Counts.InterlockedAdd(56,1);
    if(r.state.z&8u)Counts.InterlockedAdd(60,1);
}
[numthreads(1,1,1)]
void ComplexityPrepare(){
    [unroll]for(uint i=0;i<8;++i)Arguments.Store3(i*12,uint3(Counts.Load((i+1)*4),1,1));
    Arguments.Store4(96,uint4(24,Counts.Load(0),0,0));
}
struct Varying {float4 position:SV_Position;float3 color:COLOR;};
float3 heat(float v){return saturate(float3(1.5-abs(4*v-3),1.5-abs(4*v-2),1.5-abs(4*v-1)));}
Varying ComplexityVS(uint vertex:SV_VertexID,uint instance:SV_InstanceID){
    static const uint ends[24]={0,1,2,3,4,5,6,7,0,2,1,3,4,6,5,7,0,4,1,5,2,6,3,7};
    uint id=Active[instance],corner=ends[vertex];Brick b=State[id];
    float3 p=MinimumSpacing.xyz+(float3(brickCoord(id))+float3(corner&1,(corner>>1)&1,corner>>2))*8*MinimumSpacing.w;
    Varying o;o.position=mul(float4(p,1),ViewProjection);
    float value=0;
    if(Control.y<=4)value=b.importance[Control.y-1];
    else if(Control.y==5)value=b.dynamics.x;
    else if(Control.y==6)value=1-b.state.x/3.0;
    else if(Control.y==7)value=1-b.state.y/3.0;
    else if(Control.y==8)value=(b.state.z&2u)?1:((b.state.z&8u)?.5:0);
    else if(Control.y==9)value=(b.state.z&4u)?1:0;
    else value=normMetric(b.dynamics.y,Scales.x);
    o.color=heat(value);return o;
}
float4 ComplexityPS(Varying i):SV_Target{return float4(i.color,1);}
