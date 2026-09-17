#ifndef FLUID_FIELD
#define FLUID_FIELD
// Included after the renderer Frame constants. Identity fluid TLAS transform;
// positions and displacements are world metres. Each brick owns 8^3 cells/9^3 nodes.
StructuredBuffer<float4> LiquidField : register(t4);
// First FluidBricks.w uints map spatial brick IDs to compact slots. The tail
// holds 17 uints/slot: 16 conservative cell masks and packed surface cell bounds.
// Then one LOD deformation word, phase-domain enable, float3 min, float3 max.
StructuredBuffer<uint> LiquidBrickMap : register(t5);
bool liquidSurfaceMoving(uint state){
    if(state&1u)return true; // Current physical motion / collider change.
    if(!(state&2u))return false; // No LOD work this frame; old flag is stale.
    return LiquidBrickMap[FluidBricks.w*18]!=0;
}
uint liquidBrickIndex(uint3 b){return (b.z*FluidBricks.y+b.y)*FluidBricks.x+b.x;}
float4 liquidNode(uint slot,uint3 p){return LiquidField[slot*729+(p.z*9+p.y)*9+p.x];}
bool liquidPhaseDomain(out float3 lower,out float3 upper) {
    lower=upper=0;
    if(LiquidBrickMap[FluidBricks.w*18+1]==0)return false;
    [unroll]for(uint a=0;a<3;++a){lower[a]=asfloat(LiquidBrickMap[FluidBricks.w*18+2+a]);
        upper[a]=asfloat(LiquidBrickMap[FluidBricks.w*18+5+a]);}
    return true;
}
float4 liquidRawSample(float3 world) {
    float3 g=(world-FluidMinimumSpacing.xyz)/FluidMinimumSpacing.w;
    if(any(g<0)||any(g>=float3(FluidBricks.xyz*8)))return float4(FluidMinimumSpacing.w*3,0,0,0);
    uint3 cell=uint3(floor(g)),b=cell/8;uint slot=LiquidBrickMap[liquidBrickIndex(b)];
    if(slot==0xffffffff)return float4(FluidMinimumSpacing.w*3,0,0,0);
    uint3 p=cell%8;float3 f=frac(g);
    float4 a=lerp(lerp(liquidNode(slot,p),liquidNode(slot,p+uint3(1,0,0)),f.x),
                  lerp(liquidNode(slot,p+uint3(0,1,0)),liquidNode(slot,p+uint3(1,1,0)),f.x),f.y);
    float4 c=lerp(lerp(liquidNode(slot,p+uint3(0,0,1)),liquidNode(slot,p+uint3(1,0,1)),f.x),
                  lerp(liquidNode(slot,p+uint3(0,1,1)),liquidNode(slot,p+1),f.x),f.y);
    return lerp(a,c,f.z);
}
float4 liquidSample(float3 world) {
    float4 value=liquidRawSample(world);float3 lo,hi;
    if(liquidPhaseDomain(lo,hi)){
        float3 box=max(lo-world,world-hi);value.x=max(value.x,max(box.x,max(box.y,box.z)));
    }
    return value;
}
float3 liquidNormal(float3 p) {
    // Symmetric subcell filtering reduces grid-face shading seams. Intersection
    // remains the unfiltered canonical field, shared by camera and photon paths.
    float e=FluidMinimumSpacing.w*.5;float3 n,lo,hi;
    if(liquidPhaseDomain(lo,hi)){
        float3 box=max(lo-p,p-hi);uint axis=box.y>box.x?1:0;axis=box.z>box[axis]?2:axis;
        if(box[axis]>=liquidRawSample(p).x){n=0;n[axis]=lo[axis]-p[axis]>p[axis]-hi[axis]?-1:1;return n;}
    }
    // For phase geometry, filter the free surface independently of the exact
    // container wall. Ordinary particle fields have no analytic domain.
    [unroll]for(uint a=0;a<3;++a){float3 v=0;v[a]=e;n[a]=liquidRawSample(p+v).x-liquidRawSample(p-v).x;}
    return dot(n,n)>1e-16?normalize(n):float3(0,1,0);
}
#endif
