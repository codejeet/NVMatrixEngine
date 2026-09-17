cbuffer SurfaceFrame : register(b0) {
    float4 FieldMinimumSpacing,SimulationMinimumCell;
    uint4 Bricks,SimulationGrid;
    float4 Reconstruction; // support radius, surface radius, particle capacity
    uint4 Collision;
    float4 LodTolerance; // phi / cell, normal chord, transition rate, displacement / cell
    uint4 LodControl; // enabled, frame, reset, flags: importance, validate, moving particles, force fine
    float4 LodCamera; // camera position, refresh interval
    float4 PhaseControl; // elapsed simulation seconds (zero on reset), reserved
};
#define FLUID_COLLIDER_REGISTER t0
#define FLUID_MESH_REGISTER t1
#include "solid.hlsli"
struct Particle {float4 positionRadius,velocityFlags,apic0,apic1,apic2;};
RWStructuredBuffer<Particle> Particles : register(u0);
RWStructuredBuffer<uint> Offsets : register(u1);
RWStructuredBuffer<uint> Indices : register(u2);
RWStructuredBuffer<float4> Previous : register(u3);
RWStructuredBuffer<uint> BrickMap : register(u4);
RWStructuredBuffer<float4> Field : register(u5);
RWStructuredBuffer<uint> BrickList : register(u6);
struct Aabb {float3 minimum,maximum;};
RWStructuredBuffer<Aabb> Bounds : register(u7);
RWByteAddressBuffer Counters : register(u8);
RWByteAddressBuffer Arguments : register(u9);
#define INTERIOR_STATE_REGISTER u11
#define INTERIOR_TOTAL_REGISTER u12
#include "interior-shared.hlsli"
uint3 brickCoord(uint i){return uint3(i%Bricks.x,(i/Bricks.x)%Bricks.y,i/(Bricks.x*Bricks.y));}
uint simIndex(int3 p){return (p.z*SimulationGrid.y+p.y)*SimulationGrid.x+p.x;}
#include "anisotropy.hlsli"
#if FLUID_PHASE_SURFACE
#include "owned-surface.hlsli"
#endif
#if FLUID_NARROW_SURFACE
#include "narrow-surface.hlsli"
#endif
[numthreads(128,1,1)]
void SurfaceClear(uint3 id:SV_DispatchThreadID) {
    if(id.x==0){
        Counters.Store(0,0);Counters.Store(4,0);Counters.Store(8,0);Counters.Store(12,0);BrickMap[Bricks.w*18]=0;
        if(PhaseControl.y!=0){Counters.Store(16,0);Counters.Store(20,0);Counters.Store(24,0);}
        // The canonical phase surface is a trilinear free-surface field clipped
        // by this exact domain, not a trilinear approximation of max(phi, box).
        BrickMap[Bricks.w*18+1]=PhaseControl.y!=0?1u:0u;
        [unroll]for(uint a=0;a<3;++a){
            BrickMap[Bricks.w*18+2+a]=asuint(SimulationMinimumCell[a]);
            BrickMap[Bricks.w*18+5+a]=asuint(SimulationMinimumCell[a]+SimulationGrid[a]*SimulationMinimumCell.w);
        }
    }
    if(id.x>=Bricks.w)return;
    BrickMap[id.x]=0xffffffff;
    Aabb a;a.minimum=float3(asfloat(0x7fc00000),0,0);a.maximum=0;Bounds[id.x]=a;
}
[numthreads(128,1,1)]
void SurfaceMark(uint3 id:SV_DispatchThreadID) {
    if(id.x>=Bricks.w)return;
#if FLUID_PHASE_SURFACE
    bool active=ownedSurfaceActive(id.x);
#else
    float3 lo=FieldMinimumSpacing.xyz+float3(brickCoord(id.x)*8)*FieldMinimumSpacing.w;
    float halo=Reconstruction.x*(Collision.y?1.2:1);
    int3 a=max(0,int3(floor((lo-halo-SimulationMinimumCell.xyz)/SimulationMinimumCell.w)));
    int3 b=min(int3(SimulationGrid.xyz)-1,int3(floor((lo+8*FieldMinimumSpacing.w+halo-SimulationMinimumCell.xyz)/SimulationMinimumCell.w)));
    bool active=Reconstruction.w!=0;
    for(int z=a.z;z<=b.z&&!active;++z)for(int y=a.y;y<=b.y&&!active;++y)for(int x=a.x;x<=b.x;++x) {
        uint i=simIndex(int3(x,y,z));if(Offsets[i+1]>Offsets[i]||(Collision.z&&interiorCellQuanta(uint3(x,y,z),SimulationGrid.xyz,SimulationMinimumCell))){active=true;break;}
    }
#if FLUID_NARROW_SURFACE
    if(!active&&all(a<=b))active=narrowSurfaceActive(a,b);
#endif
#endif
    if(active){uint slot;Counters.InterlockedAdd(0,1,slot);BrickMap[id.x]=slot;BrickList[slot]=id.x;}
}
[numthreads(1,1,1)]
void SurfacePrepare(){
    uint active=Counters.Load(0);
    Arguments.Store3(0,uint3(active*6,1,1));
    Arguments.Store3(12,uint3(active,1,1));
    if(LodControl.x){Arguments.Store3(24,uint3(Counters.Load(16)*6,1,1));Arguments.Store3(36,uint3(Counters.Load(20),1,1));}
}
float4 reconstructNode(uint brick,uint3 local) {
    uint3 v=local+brickCoord(brick)*8;
#if FLUID_PHASE_SURFACE
    return reconstructOwnedNode(v);
#else
    float3 p=FieldMinimumSpacing.xyz+float3(v)*FieldMinimumSpacing.w;
    if(Reconstruction.w!=0) {
        float phi=Reconstruction.w==1?length(p-float3(3.7,1.99,-3.1))-.75:
            max(abs(p.y-1.99)-.01,max(abs(p.x-3.7)-1,abs(p.z+3.1)-1));
        if(Reconstruction.w>=3){float3 q=abs(p-float3(3.7,1.99,-3.1))-float3(1.6,.6,1.6);
            if(Reconstruction.w==4)q.y+=.01*sin(p.x*1.3)*cos(p.z*.9);
            phi=max(q.x,max(q.y,q.z));}
        return float4(phi,0,0,0);
    }
    float support=Reconstruction.x;
    float halo=support*(Collision.y?1.2:1);
    int3 a=max(0,int3(floor((p-halo-SimulationMinimumCell.xyz)/SimulationMinimumCell.w)));
    int3 b=min(int3(SimulationGrid.xyz)-1,int3(floor((p+halo-SimulationMinimumCell.xyz)/SimulationMinimumCell.w)));
    float wsum=0;float3 offset=0,motion=0;
    float3x3 metric=0;
    for(int z=a.z;z<=b.z;++z)for(int y=a.y;y<=b.y;++y)for(int x=a.x;x<=b.x;++x) {
        uint cell=simIndex(int3(x,y,z));
        for(uint j=Offsets[cell];j<Offsets[cell+1];++j) {
            uint index=Indices[j];float3 q=Particles[index].positionRadius.xyz-p;
            if(dot(q,q)>halo*halo)continue;
            ParticleShape shape=Shapes[index];float3x3 g=shapeMatrix(shape);q+=shapeOffset(shape);
            float3 anisotropicQ=mul(g,q);
            float w=max(0,1-dot(anisotropicQ,anisotropicQ)/(support*support));
            if(w==0)continue; // No determinant, mass or history loads outside the ellipsoid.
            w=w*w*w*determinant(g)*Particles[index].apic0.w;
            wsum+=w;offset+=q*w;motion+=(Previous[index].xyz-Particles[index].positionRadius.xyz)*w;
            metric+=g*w;
        }
    }
    // Zhu/Bridson-style weighted center field. Smooth shared node values across
    // brick boundaries; isolated droplets remain closed. Phi is NOT an SDF.
#if FLUID_NARROW_SURFACE
    if(all(a<=b))narrowSurfaceGather(p,a,b,wsum,offset,motion,metric);
#endif
    float phi=wsum>1e-8?length(mul(metric/wsum,offset/wsum))-Reconstruction.y:support;
#ifdef FLUID_NARROW_SURFACE
    if(wsum>1e-8){
        // The mean anisotropy matrix is symmetric. Its maximum absolute row
        // sum bounds its stretch. Divide the scalar by that bound: an isolated
        // small owner keeps the SAME zero surface without producing million-
        // fold gradients in the nodal field. DDA/root tracing stays canonical.
        float3x3 m=metric/wsum;
        float stretch=max(dot(abs(m[0]),float3(1,1,1)),
                          max(dot(abs(m[1]),float3(1,1,1)),dot(abs(m[2]),float3(1,1,1))));
        phi/=max(1,stretch);
    }
#endif
    if(Collision.z)phi=min(phi,interiorPhi(p,SimulationMinimumCell,SimulationGrid.xyz));
    for(uint i=0;i<Collision.x;++i)phi=max(phi,.002-colliderPhi(Colliders[i],p));
    if(LodControl.x&&!(LodControl.w&4u))motion=0;
    return float4(phi,wsum>1e-8?motion/wsum:0);
#endif
}
[numthreads(128,1,1)]
void SurfaceReconstruct(uint3 group:SV_GroupID,uint lane:SV_GroupIndex) {
    uint slot=group.x/6,node=(group.x%6)*128+lane;if(node>=729)return;
    Field[slot*729+node]=reconstructNode(BrickList[slot],uint3(node%9,(node/9)%9,node/81));
}
#include "surface-lod.hlsli"
groupshared uint3 SurfaceMin[128],SurfaceMax[128];
groupshared uint SurfaceMask[16];
float tetraFraction(float4 phi) {
    float negative[4],positive[4];uint ni=0,no=0;
    [unroll]for(uint i=0;i<4;++i){if(phi[i]<0)negative[ni++]=phi[i];else positive[no++]=phi[i];}
    if(ni==0)return 0;if(ni==4)return 1;
    if(ni==1){float3 t=negative[0]/(negative[0]-float3(positive[0],positive[1],positive[2]));return t.x*t.y*t.z;}
    if(ni==3){float3 t=positive[0]/(positive[0]-float3(negative[0],negative[1],negative[2]));return 1-t.x*t.y*t.z;}
    float u=negative[0]/(negative[0]-positive[0]),v=negative[0]/(negative[0]-positive[1]);
    float s=negative[1]/(negative[1]-positive[0]),t=negative[1]/(negative[1]-positive[1]);
    return u*v+u*t*(1-v)+s*t*(1-u);
}
[numthreads(128,1,1)]
void SurfaceBounds(uint3 group:SV_GroupID,uint lane:SV_GroupIndex) {
    uint slot=group.x;if(slot>=Counters.Load(0))return;
    if(lane<16)SurfaceMask[lane]=0;
    GroupMemoryBarrierWithGroupSync();
    uint3 lower=8,upper=0;
    for(uint n=lane;n<729;n+=128){float4 value=Field[slot*729+n];if(any(!isfinite(value)))Counters.InterlockedAdd(8,1);}
    uint volume=0;
    for(uint n=lane;n<512;n+=128){uint3 cell=uint3(n%8,(n/8)%8,n/64);float phi[8];float minPhi=1e6,maxPhi=-1e6;
        float minContour=1e6,maxContour=-1e6;
        int3 domainCell=int3(brickCoord(BrickList[slot])*8+cell)-4;
        [unroll]for(uint k=0;k<8;++k){uint3 bit=uint3(k&1,(k>>1)&1,k>>2),p=cell+bit;
            phi[k]=Field[slot*729+(p.z*9+p.y)*9+p.x].x;minPhi=min(minPhi,phi[k]);maxPhi=max(maxPhi,phi[k]);
            float contour=phi[k];
            if(PhaseControl.y!=0){int3 node=domainCell+int3(bit);
                int3 box=max(-node,node-int3(SimulationGrid.xyz*2));
                contour=max(contour,float(max(box.x,max(box.y,box.z)))*FieldMinimumSpacing.w);}
            minContour=min(minContour,contour);maxContour=max(maxContour,contour);
        }
        // Trilinear interpolation is a convex combination of its corners. A
        // strict common sign excludes ALL roots, including subcell double hits.
        if(minContour<=0&&maxContour>=0){InterlockedOr(SurfaceMask[n/32],1u<<(n%32));lower=min(lower,cell);upper=max(upper,cell+1);}
        // Domain walls coincide with field lattice planes. Integrate the raw
        // scalar only in interior cells; nodal domain clamping would bias
        // oblique clipped volumes at wall/free-surface junctions.
        if(PhaseControl.y!=0 && (any(domainCell<0) || any(domainCell+1>int3(SimulationGrid.xyz*2))))continue;
        // Nonpositive corners with any strict negative corner imply negative
        // trilinear phi throughout the open cell (all interpolation weights
        // are positive). A tetrahedral estimate can incorrectly discard flat
        // zero tetrahedra at exact box edges/corners; handle this case exactly.
        if(maxPhi<=0&&minPhi<0)volume+=256;
        else if(minPhi<0){
            // Diagnostic volume estimate only: integrate six linear tetrahedra.
            // Unlike a cell-center occupancy count, this sees subcell sheets.
            float fraction=tetraFraction(float4(phi[0],phi[1],phi[3],phi[7]))+tetraFraction(float4(phi[0],phi[3],phi[2],phi[7]))+
                tetraFraction(float4(phi[0],phi[2],phi[6],phi[7]))+tetraFraction(float4(phi[0],phi[6],phi[4],phi[7]))+
                tetraFraction(float4(phi[0],phi[4],phi[5],phi[7]))+tetraFraction(float4(phi[0],phi[5],phi[1],phi[7]));
            volume+=uint(round(saturate(fraction/6)*256));
        }}
    if(volume)Counters.InterlockedAdd(12,volume);
    SurfaceMin[lane]=lower;SurfaceMax[lane]=upper;GroupMemoryBarrierWithGroupSync();
    if(lane<16)BrickMap[Bricks.w+slot*17+lane]=SurfaceMask[lane];
    for(uint stride=64;stride;stride>>=1){if(lane<stride){SurfaceMin[lane]=min(SurfaceMin[lane],SurfaceMin[lane+stride]);SurfaceMax[lane]=max(SurfaceMax[lane],SurfaceMax[lane+stride]);}GroupMemoryBarrierWithGroupSync();}
    if(lane==0&&all(SurfaceMin[0]<SurfaceMax[0])) {
        uint3 lo=SurfaceMin[0],hi=SurfaceMax[0];
        BrickMap[Bricks.w+slot*17+16]=lo.x|(lo.y<<4)|(lo.z<<8)|(hi.x<<12)|(hi.y<<16)|(hi.z<<20);
        if(LodControl.x){float blend=SurfaceLodNext[BrickList[slot]].error.w;
            BrickMap[Bricks.w+slot*17+16]|=uint(round(blend*255))<<24;
            if(blend==1)Counters.InterlockedAdd(24,1);else if(blend>0)Counters.InterlockedAdd(28,1);}
        uint id=BrickList[slot];Aabb a;
        // Bound surface cells, not the entire brick. Pad for fp32 coordinate
        // rounding; canonical phi, root refinement and shading normals are unchanged.
        float3 origin=FieldMinimumSpacing.xyz+float3(brickCoord(id)*8)*FieldMinimumSpacing.w;
        a.minimum=origin+float3(SurfaceMin[0])*FieldMinimumSpacing.w-FieldMinimumSpacing.w*.001;
        a.maximum=origin+float3(SurfaceMax[0])*FieldMinimumSpacing.w+FieldMinimumSpacing.w*.001;
        Bounds[id]=a;Counters.InterlockedAdd(4,1);
    }
}
