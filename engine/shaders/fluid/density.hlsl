#include "common.hlsli"
#include "work-shared.hlsli"
#ifndef BULK_PRESSURE
#define BULK_PRESSURE 0
#endif
#if BULK_PRESSURE
RWStructuredBuffer<float> BulkFilledVolumeFraction : register(u36);
#endif
RWStructuredBuffer<uint> DensityArguments : register(u28);
#if FLUID_NARROW_DENSITY
// Validation-only root binding replaces the particle-ledger slot. Production
// CUDA density uses the same integrated uniform-cell basis for grid owners.
RWStructuredBuffer<double4> NarrowDensityGrid : register(u37);
#endif
#if CUT_PRESSURE
#include "density-kernel.hlsli"
RWStructuredBuffer<FluidSolidKernel> DensitySolidKernel : register(u35);
#endif
// Positional density projection, independent of the divergence-free velocity
// projection. An initial unilateral (compression-only) variant inspired by
// Kugelstadt et al. 2019. This is not their full multiphase/solid treatment.
float particleDensity(int3 c) {
    float3 center=float3(c)+.5;
    float mass=0;
    for(int z=-1;z<=1;++z)for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x) {
        int3 n=c+int3(x,y,z);if(!inGrid(n))continue;
        uint b=cellIndex(n);
#if FLUID_NARROW_DENSITY
        uint3 nc=uint3(n)/2, extent=min(2,Grid.xyz-nc*2), dims=(Grid.xyz+1)/2;
        uint owner=(nc.z*dims.y+nc.y)*dims.x+nc.x;
        float w=(x==0?2.0/3:1.0/6)*(y==0?2.0/3:1.0/6)*(z==0?2.0/3:1.0/6);
        mass+=float(NarrowDensityGrid[owner].w/((double)(extent.x*extent.y*extent.z)*(double)InitialMinimum.w))*w;
#endif
        for(uint j=CellOffsets[b];j<CellOffsets[b+1];++j) {
            FluidParticle p=Particles[SortedIndices[j]];
            float3 q=(p.positionRadius.xyz-DomainMinCell.xyz)/DomainMinCell.w-center;
            mass+=quadratic(q.x)*quadratic(q.y)*quadratic(q.z)*particleWeight(p);
        }
    }
    if(Display.z)mass+=interiorGather(center,DomainMinCell,Grid.xyz,0).y;
    return mass*Solver.w;
}
// Validation only: current positions, including final collisions and ownership
// exchanges. Preserve XYZ (the pre-correction solver diagnostic), and do not
// clear pressure/scratch or modify any simulation state consumed next frame.
[numthreads(128,1,1)]
void DensityMeasure(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x;if(id>=Grid.w)return;
    Density[id].w=particleDensity(int3(cellFromIndex(id)));
}
bool densityOccupied(uint id) {
#if BULK_PRESSURE
    // Retain the filled interior's pressure connectivity during positional
    // correction too. The RHS still contains only measured particle/solid
    // kernel mass: support must not introduce a second copy of fluid mass.
    return cellOccupied(id)||BulkFilledVolumeFraction[id]>0;
#else
    return cellOccupied(id);
#endif
}
void gatherDensity(uint id,bool adaptive) {
    if(id>=Grid.w)return;
    int3 c=int3(cellFromIndex(id));float mass=particleDensity(c);
    // Integral of the missing quadratic kernel beyond a flat domain wall is
    // 1/6 at the first cell center. Fill that *solid* portion, never air.
#if CUT_PRESSURE
    // Cached on actual collider endpoint changes, not once per density repair.
    float solidMass=DensitySolidKernel[id].mass;
#else
    float solidMass=0;
    [unroll]for(int sz=-1;sz<=1;++sz)[unroll]for(int sy=-1;sy<=1;++sy)[unroll]for(int sx=-1;sx<=1;++sx)
        if(solid(c+int3(sx,sy,sz)))solidMass+=(sx==0?2.0/3:1.0/6)*(sy==0?2.0/3:1.0/6)*(sz==0?2.0/3:1.0/6);
#endif
    float rho=mass+solidMass;
    // Solve the full unilateral density error. Halving it before an already
    // displacement-limited projection leaves sustained wall compression under
    // moving boundaries. Bound the linearization; the particle step below has
    // its own independent 0.2h trust region and is collision-projected again.
    float rhs=min(max(rho-1,0),.5);
    // Retain the original pre-correction diagnostic even if a repair is needed.
    if(!adaptive)Density[id]=float4(mass,rhs,!solid(c)&&densityOccupied(id)?1:0,0);
    if(adaptive&&!solid(c)&&densityOccupied(id)&&rho>1.02)InterlockedOr(DensityArguments[24],1);
    PressureIn[id]=0;PressureOut[id]=0;
    uint mask=0,diagonal=0;
    if(!solid(c)&&densityOccupied(id)) {
        [unroll]for(uint axis=0;axis<3;++axis)[unroll]for(int side=-1;side<=1;side+=2) {
            int3 n=c;n[axis]+=side;
#if CUT_PRESSURE
            int3 face=c;if(side>0)face[axis]++;
            if(cutPressureAperture(face,axis)==0)continue;
#endif
            if(!solid(n)){diagonal++;if(densityOccupied(cellIndex(n)))mask|=1u<<(axis*2+(side>0?1:0));}
        }
    }
    FaceScratch[id]=float4(DomainMinCell.w*DomainMinCell.w*rhs,diagonal>0?1.0/diagonal:0,float(mask),0);
}
[numthreads(128,1,1)]void DensityGather(uint3 tid:SV_DispatchThreadID){gatherDensity(tid.x,false);}
[numthreads(128,1,1)]void DensityGatherAdaptive(uint3 tid:SV_DispatchThreadID){gatherDensity(tid.x,true);}
[numthreads(64,1,1)]void DensityClearArguments(uint id:SV_DispatchThreadID){DensityArguments[id]=0;}
[numthreads(64,1,1)]void DensityContinueArguments(uint id:SV_DispatchThreadID){if(id!=25)DensityArguments[id]=0;}
[numthreads(1,1,1)]void DensityPrepareArguments(){
    DensityArguments[25]+=DensityArguments[24]?1u:0u;
    // Six conditional rebin stages, Jacobi, and particle displacement/contact.
    uint groups[8]={(Grid.w+255)/256,(Counts.x+255)/256,(Grid.w+255)/256,1,
                    (Grid.w+255)/256,(Counts.x+255)/256,(Grid.w+255)/256,(Counts.x+127)/128};
    for(uint i=0;i<8;++i){DensityArguments[3*i]=DensityArguments[24]?groups[i]:0;
        DensityArguments[3*i+1]=1;DensityArguments[3*i+2]=1;}
    uint3 tiles=(Grid.xyz+uint3(7,3,3))/uint3(8,4,4);
    uint total=tiles.x*tiles.y*tiles.z;
    DensityArguments[30]=DensityArguments[24]?min(total,65535u):0;
    DensityArguments[31]=(total+65534)/65535;DensityArguments[32]=1;
}
[numthreads(256,1,1)]
void DensityJacobi(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x;if(id>=Grid.w)return;
    PressureOut[id]=pressureStencil(id);
}
// Same exact two global Jacobi iterates, using the existing pressure tiling
// scheme. Halo values read ONLY PressureIn; no inter-group synchronization or
// stale neighbor updates. Dense tile scheduling is retained for now.
groupshared float DensityFirstSweep[360];
void densityTile(uint tile,uint lane) {
    uint3 tiles=(Grid.xyz+uint3(7,3,3))/uint3(8,4,4);
    if(tile>=tiles.x*tiles.y*tiles.z)return;
    int3 base=int3(uint3(tile%tiles.x,(tile/tiles.x)%tiles.y,tile/(tiles.x*tiles.y))*uint3(8,4,4));
    for(uint node=lane;node<360;node+=128) {
        int3 p=base+int3(node%10,(node/10)%6,node/60)-1;
        DensityFirstSweep[node]=inGrid(p)?pressureStencil(cellIndex(p)):0;
    }
    GroupMemoryBarrierWithGroupSync();
    uint3 local=uint3(lane%8,(lane/8)%4,lane/32),p=uint3(base)+local;
    if(any(p>=Grid.xyz))return;
    uint id=cellIndex(p),here=((local.z+1)*6+local.y+1)*10+local.x+1;
    float4 s=FaceScratch[id];uint mask=uint(s.z),stride[3]={1,10,60};float sum=0;
    [unroll]for(uint a=0;a<3;++a) {
        if(mask&(1u<<(a*2)))sum+=DensityFirstSweep[here-stride[a]];
        if(mask&(2u<<(a*2)))sum+=DensityFirstSweep[here+stride[a]];
    }
    PressureOut[id]=(sum+s.x)*s.y;
}
[numthreads(128,1,1)]void DensityTileJacobi(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){densityTile(workGroup(group),lane);}
[numthreads(128,1,1)]void DensityTileSparse(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    uint id=workGroup(group);if(id>=SimulationWork[1])return;
    densityTile(SimulationWork[workDensityList()+id],lane);
}
float displacementFace(int3 right,uint axis) {
    int3 left=right;left[axis]--;
#if CUT_PRESSURE
    if(cutPressureAperture(right,axis)==0)return 0;
#else
    if(solid(left)||solid(right))return 0;
#endif
    return -(PressureIn[cellIndex(right)]-PressureIn[cellIndex(left)])/DomainMinCell.w;
}
[numthreads(128,1,1)]
void DensityDisplace(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x;if(id>=Counts.x)return;
    FluidParticle p=Particles[id];if(!p.velocityFlags.w)return;
    float3 displacement=0;
    for(uint a=0;a<3;++a) {
        float3 gp=(p.positionRadius.xyz-DomainMinCell.xyz)/DomainMinCell.w-faceOffset(a);
        int3 base=int3(floor(gp-.5));
        for(int z=0;z<3;++z)for(int y=0;y<3;++y)for(int x=0;x<3;++x) {
            int3 c=base+int3(x,y,z);float3 q=float3(c)-gp;
            displacement[a]+=quadratic(q.x)*quadratic(q.y)*quadratic(q.z)*displacementFace(c,a);
        }
    }
    displacement*=min(1,.2*DomainMinCell.w/max(length(displacement),1e-10));
    p.positionRadius.xyz=clamp(p.positionRadius.xyz+displacement,DomainMinCell.xyz+p.positionRadius.w,DomainMaxRadius.xyz-p.positionRadius.w);
    Particles[id]=p; // No velocity/affine injection, no spawning/deleting particles.
}
