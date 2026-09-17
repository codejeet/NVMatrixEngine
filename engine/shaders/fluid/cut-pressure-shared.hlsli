#ifndef FLUID_CUT_PRESSURE_SHARED
#define FLUID_CUT_PRESSURE_SHARED
RWStructuredBuffer<float2> CutVolume : register(u31);
RWStructuredBuffer<float> CutAperture : register(u32);
cbuffer CutProjectionFrame : register(b2){uint CutSwept;}; // bit 0 swept source, bit 1 temporal pressure support
float cutPressureCapacity(int3 p){float2 v=CutVolume[cellIndex(p)];return (CutSwept&2)?.5*(v.x+v.y):v.x;}
float3 cutPressureWidths(int3 p){return min(DomainMinCell.w,max(0,DomainMaxRadius.xyz-(DomainMinCell.xyz+float3(p)*DomainMinCell.w)));}
float cutPressureVolume(int3 p){return cutPressureCapacity(p)/pow(DomainMinCell.w,3);}
float cutPressureSource(int3 p){float2 v=CutVolume[cellIndex(p)];return (CutSwept&1)?(v.x-v.y)/(GravityDt.w*DomainMinCell.w*DomainMinCell.w):0;}
float cutPressureAperture(int3 right,uint axis){
    int3 left=right;left[axis]--;
    // Geometric aperture is not permission to leave the closed outer domain.
    if(!inGrid(left)||!inGrid(right))return 0;
    if(cutPressureCapacity(left)==0||cutPressureCapacity(right)==0)return 0;
    return CutAperture[faceIndex(right,axis)]/(DomainMinCell.w*DomainMinCell.w);
}
float cutPressureDistance(int3 right,uint axis){int3 left=right;left[axis]--;
    return .5*(cutPressureWidths(left)[axis]+cutPressureWidths(right)[axis])/DomainMinCell.w;
}
double cutPressureWidthPrecise(int3 p,uint axis){
    double width=double(DomainMaxRadius[axis])-(double(DomainMinCell[axis])+double(p[axis])*double(DomainMinCell.w));
    return width<0?0:width>double(DomainMinCell.w)?double(DomainMinCell.w):width;
}
double cutPressureDistancePrecise(int3 right,uint axis){int3 left=right;left[axis]--;
    return .5*(cutPressureWidthPrecise(left,axis)+cutPressureWidthPrecise(right,axis))/double(DomainMinCell.w);
}
double cutPressureAperturePrecise(int3 right,uint axis){
    int3 left=right;left[axis]--;
    if(!inGrid(left)||!inGrid(right))return 0;
    if(cutPressureCapacity(left)==0||cutPressureCapacity(right)==0)return 0;
    return double(CutAperture[faceIndex(right,axis)])/(double(DomainMinCell.w)*double(DomainMinCell.w));
}
float cutPressureBoundary(int3 right,uint axis){
    int3 left=right;left[axis]--;if(!inGrid(left)||!inGrid(right))return 0;
    float3 world=min(DomainMaxRadius.xyz,DomainMinCell.xyz+(float3(right)+faceOffset(axis))*DomainMinCell.w);
    float distance=1e6,velocity=0;
    for(uint k=0;k<Collision.x;++k){float phi=colliderPhi(Colliders[k],world);if(phi<distance){distance=phi;velocity=colliderVelocity(Colliders[k],world)[axis];}}
    return velocity;
}
#endif
