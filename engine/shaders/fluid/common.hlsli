#ifndef FLUID_COMMON
#define FLUID_COMMON
cbuffer FluidFrame : register(b0) {
    float4 DomainMinCell,DomainMaxRadius,GravityDt;
    uint4 Counts;
    row_major float4x4 ViewProjection;
    float4 CameraRight,CameraUp,CameraPosition,CameraForward;
    uint4 Grid;
    float4 Solver; // density kg/m^3, FLIP fraction, transfer mode, particle volume / cell volume
    uint4 Display;
    uint4 Collision;
    float4 Material; // explicit diffusion alpha, surface tension N/m, subcycles, fixture
    float4 InitialMinimum,InitialMaximum;
    uint4 InitialLattice,Emission; // room layout; first slot, birth count, serial, reserved
    float4 EmitterOriginRadius,EmitterVelocity;
};
struct FluidParticle {float4 positionRadius,velocityFlags,apic0,apic1,apic2;};
// apic0.w is rest-mass multiplicity relative to one emitted particle. Dyadic
// weights are counted in 1/16 quanta; XYZ affine rows retain their original units.
float particleWeight(FluidParticle p){return p.apic0.w;}
#if FLUID_PARTICLE_AUTHORITY
RWStructuredBuffer<double4> ParticleQuantity : register(u37);
#endif
RWStructuredBuffer<FluidParticle> Particles : register(u0);
RWStructuredBuffer<uint> CellCounts : register(u1);
RWStructuredBuffer<uint> CellOffsets : register(u2);
RWStructuredBuffer<uint> CellCursor : register(u3);
RWStructuredBuffer<uint> SortedIndices : register(u4);
RWStructuredBuffer<uint> ScanBlocks : register(u5);
// Contiguous U/V/W face planes with a padded (Nx+1)*(Ny+1)*(Nz+1)
// stride. xyz = projected velocity, pre-force/P2G velocity, interpolation mass.
RWStructuredBuffer<float4> Faces : register(u6);
RWStructuredBuffer<float> PressureIn : register(u7);
RWStructuredBuffer<float> PressureOut : register(u8);
// divergence before/after projection, type (0 air / 1 liquid), reserved
RWStructuredBuffer<float4> Cells : register(u9);
RWStructuredBuffer<float4> FaceScratch : register(u10);
// Between diffusion and extrapolation, then again during density projection,
// the first Grid.w scratch entries cache (RHS, inverse diagonal, neighbor mask).
// These lifetimes do not overlap face ping-pong. No new grid buffer is needed.
RWStructuredBuffer<float4> Density : register(u11);
RWStructuredBuffer<float4> PreviousPositions : register(u12);
RWStructuredBuffer<float4> SolidGrid : register(u13); // phi, solid velocity xyz
RWStructuredBuffer<float4> MaterialGrid : register(u14); // colour, outward curvature, gradient magnitude, reserved
RWStructuredBuffer<uint> CellQuanta : register(u15);
#include "solid.hlsli"
#include "interior-shared.hlsli"
uint faceStride(){return (Grid.x+1)*(Grid.y+1)*(Grid.z+1);}
uint faceIndex(uint3 p,uint axis){return axis*faceStride()+(p.z*(Grid.y+1)+p.y)*(Grid.x+1)+p.x;}
float3 faceOffset(uint axis){float3 p=.5;p[axis]=0;return p;}
float quadratic(float x){x=abs(x);return x<.5?.75-x*x:(x<1.5?.5*(1.5-x)*(1.5-x):0);}
float3 testAffine(uint axis){return axis==0?float3(.2,-.4,.1):(axis==1?float3(.4,-.1,.2):float3(0,-.2,-.1));}
uint3 cellCoord(float3 p) {return min(uint3(max(0,(p-DomainMinCell.xyz)/DomainMinCell.w)),Grid.xyz-1);}
uint cellIndex(uint3 p) {return (p.z*Grid.y+p.y)*Grid.x+p.x;}
uint3 cellFromIndex(uint id){return uint3(id%Grid.x,(id/Grid.x)%Grid.y,id/(Grid.x*Grid.y));}
bool inGrid(int3 c){return all(c>=0)&&all(c<int3(Grid.xyz));}
#ifndef CUT_PRESSURE
#define CUT_PRESSURE 0
#endif
#if CUT_PRESSURE
#include "cut-pressure-shared.hlsli"
#endif
bool liquid(int3 c){return inGrid(c)?Cells[cellIndex(c)].z==1:false;}
bool solid(int3 c){
#if CUT_PRESSURE
    return !inGrid(c)||cutPressureCapacity(c)==0;
#else
    return !inGrid(c)||SolidGrid[cellIndex(c)].x<0;
#endif
}
// Mode 1 is the existing dyadic resampler. Mode 2 stores the rounded FP32
// sum of authoritative particle mass, not quantized 1/16 mass units.
float cellMass(uint id){return Display.w==2?asfloat(CellQuanta[id]):(Display.w!=0?CellQuanta[id]*(1.0/16):float(CellCounts[id]));}
bool cellOccupied(uint id){return CellCounts[id]>0||(Display.z!=0&&CellQuanta[id]>0);}
float boundaryVelocity(int3 left,int3 right,uint axis) {
    if(!inGrid(left)||!inGrid(right))return 0;
    return SolidGrid[cellIndex(solid(left)?left:right)][axis+1];
}
// V1 full-cell free surface: boundary pressure sigma*kappa. The pressure solve
// and MAC gradient MUST use exactly the same ghost value to stay compatible.
float capillaryPressure(int3 liquidCell,int3 airCell) {
    float2 a=MaterialGrid[cellIndex(liquidCell)].xy,b=MaterialGrid[cellIndex(airCell)].xy;
    float t=abs(a.x-b.x)>1e-6?saturate((a.x-.5)/(a.x-b.x)):.5;
    return Material.y*lerp(a.y,b.y,t);
}
float pressureStencil(uint id) {
    float4 stencil=FaceScratch[id];uint mask=uint(stencil.z);
    float sum=0;uint strides[3]={1,Grid.x,Grid.x*Grid.y};
    [unroll]for(uint axis=0;axis<3;++axis) {
        if(mask&(1u<<(axis*2)))sum+=PressureIn[id-strides[axis]];
        if(mask&(2u<<(axis*2)))sum+=PressureIn[id+strides[axis]];
    }
    return (sum+stencil.x)*stencil.y;
}
#endif
