#ifndef FLUID_SOLID
#define FLUID_SOLID
#include "../ocean-terrain.hlsli"
#ifndef FLUID_COLLIDER_REGISTER
#define FLUID_COLLIDER_REGISTER t1
#define FLUID_MESH_REGISTER t2
#endif
struct FluidCollider {
    row_major float4x4 worldToLocal;
    float4 centerRestitution,extentType,velocityFriction,angularSlip;
    float4 meshMinimumSpacing;
    uint4 meshDimensions;
};
StructuredBuffer<FluidCollider> Colliders : register(FLUID_COLLIDER_REGISTER);
StructuredBuffer<float> MeshPhi : register(FLUID_MESH_REGISTER);
float colliderPhi(FluidCollider c,float3 world,float clearance) {
    float3 p=mul(float4(world,1),c.worldToLocal).xyz;
    float3 e=c.extentType.xyz;uint type=uint(c.extentType.w);
    if(type==0)return length(p)-e.x;
    if(type==1){float3 q=abs(p)-e;return length(max(q,0))+min(max(q.x,max(q.y,q.z)),0);}
    if(type==2){p.y-=clamp(p.y,-e.y,e.y);return length(p)-e.x;}
    if(type==3){float2 q=float2(length(p.xz)-e.x,abs(p.y)-e.y);return length(max(q,0))+min(max(q.x,q.y),0);}
    if(type==4)return p.y;
    if(type==6)return (p.y-oceanTerrainHeight(p.x,p.z))/1.5;
    float3 g=(p-c.meshMinimumSpacing.xyz)/c.meshMinimumSpacing.w;
    float3 q=clamp(g,0,float3(c.meshDimensions.xyz-1));
    // Imported volumes include two positive padding voxels on every side.
    // The padded-box distance is only a rejection bound, not a contact
    // surface. Ocean particles exceed the two-voxel padding: using that
    // bound as their distance/normal creates a phantom box around the hull.
    // Keep the eight-load rejection when the entire requested clearance is
    // outside; otherwise sample the mesh, including the normal's halo.
    float outside=length(g-q)*c.meshMinimumSpacing.w;
    if(any(g!=q)&&2*c.meshMinimumSpacing.w+outside>=clearance)
        return 2*c.meshMinimumSpacing.w+outside;
    uint3 cell=min(uint3(q),c.meshDimensions.xyz-2);float3 f=q-cell;
    float phi=0;
    [unroll]for(uint i=0;i<8;++i){uint3 a=uint3(i&1,(i>>1)&1,i>>2),v=cell+a;
        float3 w=lerp(1-f,f,float3(a));
        phi+=MeshPhi[c.meshDimensions.w+(v.z*c.meshDimensions.y+v.y)*c.meshDimensions.x+v.x]*w.x*w.y*w.z;}
    return phi+outside;
}
// Occupancy and conservative broad-phase queries only need the zero set.
float colliderPhi(FluidCollider c,float3 world) {return colliderPhi(c,world,0);}
float3 colliderVelocity(FluidCollider c,float3 p) {
    return c.velocityFriction.xyz+cross(c.angularSlip.xyz,p-c.centerRestitution.xyz);
}
#endif
