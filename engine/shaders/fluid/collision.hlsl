#include "common.hlsli"
[numthreads(256,1,1)]
void Snapshot(uint3 tid:SV_DispatchThreadID) {
    if(tid.x<Counts.x)PreviousPositions[tid.x]=Particles[tid.x].positionRadius;
}
[numthreads(256,1,1)]
void BakeSolids(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x;if(id>=Grid.w)return;
    float3 p=DomainMinCell.xyz+(float3(cellFromIndex(id))+.5)*DomainMinCell.w;
    float4 s=float4(1e6,0,0,0);
    for(uint i=0;i<Collision.x;++i){FluidCollider c=Colliders[i];float d=colliderPhi(c,p);if(d<s.x)s=float4(d,colliderVelocity(c,p));}
    SolidGrid[id]=s;
}
[numthreads(128,1,1)]
void Collide(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x;if(id>=Counts.x)return;
    FluidParticle p=Particles[id];if(p.velocityFlags.w!=1)return;
    float e=DomainMinCell.w*.01,clearance=p.positionRadius.w+2*e;
    // Two sweeps resolve intersections between neighboring solids. CFL-sized
    // substeps are required; this is SDF projection, not swept CCD.
    for(uint sweep=0;sweep<2;++sweep)for(uint i=0;i<Collision.x;++i) {
        FluidCollider c=Colliders[i];float d=colliderPhi(c,p.positionRadius.xyz,clearance);
        // A projected particle can land a few ULPs inside the surface. Do not
        // apply restitution/slip again for that numerical contact on sweep two.
        // The sub-micron dead band is relative to the simulation cell size.
        float contactTolerance=max(1e-7,DomainMinCell.w*1e-5);
        if(d>=p.positionRadius.w-contactTolerance)continue;
        float3 n;
        [unroll]for(uint a=0;a<3;++a){float3 v=0;v[a]=e;n[a]=colliderPhi(c,p.positionRadius.xyz+v,clearance)-colliderPhi(c,p.positionRadius.xyz-v,clearance);}
        n=dot(n,n)>1e-15?normalize(n):float3(0,1,0);
        float3 contact=p.positionRadius.xyz+n*(p.positionRadius.w-d);
        float3 lo=DomainMinCell.xyz+p.positionRadius.w,hi=DomainMaxRadius.xyz-p.positionRadius.w;
        bool blocked=any(contact<lo)||any(contact>hi);
        for(uint other=0;other<Collision.x&&!blocked;++other)
            blocked=colliderPhi(Colliders[other],contact,clearance)<p.positionRadius.w-1e-5;
        if(blocked) {
            // A floor-mounted solid's nearest face can be below the domain.
            // Find the shortest feasible escape instead of clamping back into it.
            float best=1e20;float3 origin=p.positionRadius.xyz;
            for(uint direction=0;direction<6;++direction) {
                float3 axis=0;axis[direction/2]=(direction&1)?1:-1;
                float travel=0;
                for(uint step=0;step<24;++step) {
                    float3 q=origin+axis*travel;
                    if(any(q<lo)||any(q>hi))break;
                    float phi=1e20;
                    for(uint solid=0;solid<Collision.x;++solid)phi=min(phi,colliderPhi(Colliders[solid],q,clearance));
                    float gap=p.positionRadius.w-phi;
                    if(gap<1e-5){if(travel<best){best=travel;contact=q;n=axis;}break;}
                    travel+=max(gap,1e-5);
                }
            }
        }
        p.positionRadius.xyz=contact;
        float3 wall=colliderVelocity(c,p.positionRadius.xyz),relative=p.velocityFlags.xyz-wall;
        float vn=dot(relative,n);
        if(vn<0){relative-=n*(1+saturate(c.centerRestitution.w))*vn;
            float3 tangent=relative-n*dot(relative,n);
            relative-=tangent*min(1,max(0,c.velocityFriction.w)*(-vn)/max(length(tangent),1e-6));}
        relative=lerp(relative,n*dot(relative,n),saturate(1-c.angularSlip.w));
        p.velocityFlags.xyz=relative+wall;
        // A contact changes the local velocity field discontinuously.
        p.apic0.xyz=p.apic1.xyz=p.apic2.xyz=0; // retain rest mass in apic0.w
    }
    p.positionRadius.xyz=clamp(p.positionRadius.xyz,DomainMinCell.xyz+p.positionRadius.w,DomainMaxRadius.xyz-p.positionRadius.w);
    Particles[id]=p;
}
