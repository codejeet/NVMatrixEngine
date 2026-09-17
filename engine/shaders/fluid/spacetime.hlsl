#include "common.hlsli"
#include "spacetime-shared.hlsli"
RWStructuredBuffer<FluidParticle> RenderParticles : register(u39);
RWStructuredBuffer<uint> SpacetimeDiagnostics : register(u40);
RWStructuredBuffer<float2> SpacetimePhase : register(u41);

// Same quadratic staggered interpolation as production G2P. The existing
// spatial B-spline is retained to preserve its APIC second-moment contract.
float3 spacetimeVelocity(float3 position) {
    float3 velocity=0;
    [unroll]for(uint a=0;a<3;a++) {
        float3 g=(position-DomainMinCell.xyz)/DomainMinCell.w-faceOffset(a);
        int3 base=int3(floor(g-.5)),last=int3(Grid.xyz)-1;last[a]++;
        for(int z=0;z<3;z++)for(int y=0;y<3;y++)for(int x=0;x<3;x++) {
            int3 cell=base+int3(x,y,z);float3 q=float3(cell)-g;
            velocity[a]+=quadratic(q.x)*quadratic(q.y)*quadratic(q.z)*Faces[faceIndex(clamp(cell,0,last),a)].x;
        }
    }
    return velocity;
}
bool spacetimeAdvection(inout float3 position,float interval) {
    float remaining=abs(interval),direction=interval<0?-1:1;
    // Frozen projected MAC field, local CFL <= 1. Shrink an RK trial if its
    // intermediate stages encounter faster flow. Never silently truncate time.
    [loop]for(uint step=0;step<128&&remaining>0;step++) {
        float3 v0=spacetimeVelocity(position);
        if(!all(isfinite(v0)))return false;
        float h=min(remaining,DomainMinCell.w/max(length(v0),1e-8));
        float3 displacement=0;bool accepted=false;
        [loop]for(uint retry=0;retry<16;retry++) {
            float signedH=h*direction;
            float3 v1=spacetimeVelocity(position+.5*signedH*v0);
            float3 v2=spacetimeVelocity(position+.75*signedH*v1);
            if(!all(isfinite(v1))||!all(isfinite(v2)))return false;
            float speed=max(length(v0),max(length(v1),length(v2)));
            if(speed*h<=DomainMinCell.w*1.00001) {
                displacement=signedH*((2.0/9)*v0+(3.0/9)*v1+(4.0/9)*v2);
                accepted=true;break;
            }
            h*=.5;
        }
        if(!accepted||h<=0)return false;
        position+=displacement;
        if(h>=remaining)return true;
        remaining-=h;
    }
    return remaining==0;
}
[numthreads(128,1,1)]void SpacetimeSeed(uint id:SV_DispatchThreadID) {
    if(id<SpacetimeControl.w)ParticleTime[SpacetimeControl.z+id]=0;
}
[numthreads(128,1,1)]void SpacetimeReset(uint id:SV_DispatchThreadID) {
    if(id<Counts.x)ParticleTime[id]=0;
    if(id<4)SpacetimeDiagnostics[id]=0;
}
[numthreads(128,1,1)]void SpacetimeAdvect(uint id:SV_DispatchThreadID) {
    if(id>=Counts.x||Spacetime.y==0)return;
    FluidParticle p=Particles[id];if(!p.velocityFlags.w)return;
    // Live large-dt solid coupling requires swept time-dependent contacts.
    // This transfer component refuses unsupported geometry instead of tunneling.
    if(Collision.x){InterlockedOr(SpacetimeDiagnostics[0],2);return;}
    float2 time=particleTimeAdvance(id,ParticleTime[id],length(p.velocityFlags.xyz));
    float3 position=p.positionRadius.xyz;
    if(!spacetimeAdvection(position,time.x)){InterlockedOr(SpacetimeDiagnostics[0],1);return;}
    if(any(position<DomainMinCell.xyz+p.positionRadius.w)||any(position>DomainMaxRadius.xyz-p.positionRadius.w)) {
        InterlockedOr(SpacetimeDiagnostics[0],4);return;
    }
    p.positionRadius.xyz=position;Particles[id]=p;ParticleTime[id]=time.y;
    InterlockedAdd(SpacetimeDiagnostics[1],1);
}
[numthreads(128,1,1)]void SpacetimeSynchronize(uint id:SV_DispatchThreadID) {
    if(id>=Counts.x)return;
    FluidParticle p=Particles[id];
    if(p.velocityFlags.w) {
        float3 position=p.positionRadius.xyz;
        if(spacetimeAdvection(position,ParticleTime[id]))p.positionRadius.xyz=position;
        else InterlockedOr(SpacetimeDiagnostics[0],8);
    }
    // Rendering cannot overwrite the stochastic simulation or its residual.
    RenderParticles[id]=p;
}
[numthreads(128,1,1)]void SpacetimePhaseFaces(uint id:SV_DispatchThreadID) {
    if(id>=3*faceStride())return;
    // The spatial kernel integrates to one; m0 = cellVolume/particleVolume
    // for uniform volume-weighted samples. These are phase/density coefficients,
    // NOT a second conserved inventory or the canonical optical surface.
    float phi=min(1,sqrt(max(0,Faces[id].z*Solver.w/Spacetime.w)));
    SpacetimePhase[id]=float2(phi,1/max(Solver.x*phi,1e-4*Solver.x));
}
