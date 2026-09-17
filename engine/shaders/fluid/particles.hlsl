#include "common.hlsli"
[numthreads(256,1,1)]
void Initialize(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x;if(id>=Counts.x)return;
    FluidParticle p=(FluidParticle)0;
    if(id<Counts.y) {
        uint n=max(Counts.z,1),x=id%n,y=(id/n)%n,z=id/(n*n);
        float3 uv=(float3(x,y,z)+.5)/n;
        float3 lo=lerp(DomainMinCell.xyz,DomainMaxRadius.xyz,float3(.25,.48,.25));
        float3 hi=lerp(DomainMinCell.xyz,DomainMaxRadius.xyz,float3(.75,.90,.75));
        p.positionRadius=float4(lerp(lo,hi,uv),DomainMaxRadius.w);p.velocityFlags.w=1;p.apic0.w=1;
        if(InitialLattice.w) {
            uint3 q=uint3(id%InitialLattice.x,id/(InitialLattice.x*InitialLattice.z),(id/InitialLattice.x)%InitialLattice.z);
            p.positionRadius.xyz=lerp(InitialMinimum.xyz,InitialMaximum.xyz,(float3(q)+.5)/float3(InitialLattice.xyz));
        }
        if(Counts.w) {
            p.velocityFlags.xyz=float3(.7,.2,-.3);
            if(Counts.w==2||Counts.w==4) {
                p.apic0.xyz=testAffine(0);p.apic1.xyz=testAffine(1);p.apic2.xyz=testAffine(2);
                float3 q=p.positionRadius.xyz-(DomainMinCell.xyz+DomainMaxRadius.xyz)*.5;
                p.velocityFlags.xyz+=float3(dot(p.apic0.xyz,q),dot(p.apic1.xyz,q),dot(p.apic2.xyz,q));
            }
            if(Counts.w==3) {
                p.apic0.xyz=float3(-1,0,0);p.apic1.xyz=float3(0,-1,0);p.apic2.xyz=float3(0,0,-1);
                p.velocityFlags.xyz=-(p.positionRadius.xyz-(DomainMinCell.xyz+DomainMaxRadius.xyz)*.5);
            }
        }
    }
    Particles[id]=p;
}
uint emissionHash(uint s){s^=s>>16;s*=0x7feb352d;s^=s>>15;s*=0x846ca68b;return s^(s>>16);}
[numthreads(256,1,1)]
void Emit(uint3 tid:SV_DispatchThreadID) {
    if(tid.x>=Emission.y)return;
    uint id=Emission.x+tid.x;if(id>=Counts.x)return;
    float3 axis=normalize(EmitterVelocity.xyz),u=normalize(cross(axis,abs(axis.y)<.9?float3(0,1,0):float3(1,0,0))),v=cross(axis,u);
    uint s=emissionHash(Emission.z+tid.x);
    float radius=sqrt((s>>8)*(1.0/16777216.0))*EmitterOriginRadius.w;
    float angle=6.28318530718*(emissionHash(s)>>8)*(1.0/16777216.0);
    FluidParticle p=(FluidParticle)0;
    // Stratify a swept disc, not a pile of coincident particles. Axial volume
    // equals birthCount * particleVolume, preserving the prescribed inlet flux.
    float length=Emission.y*InitialMinimum.w/(3.14159265359*EmitterOriginRadius.w*EmitterOriginRadius.w);
    float along=(tid.x+.5)/max(Emission.y,1u)*length;
    p.positionRadius=float4(EmitterOriginRadius.xyz+radius*(u*cos(angle)+v*sin(angle))+axis*along,DomainMaxRadius.w);
    p.positionRadius.xyz=clamp(p.positionRadius.xyz,DomainMinCell.xyz+p.positionRadius.w,DomainMaxRadius.xyz-p.positionRadius.w);
    p.velocityFlags=float4(EmitterVelocity.xyz,1);p.apic0.w=1;
    Particles[id]=p;PreviousPositions[id]=p.positionRadius;
}
[numthreads(256,1,1)]
void Integrate(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x;if(id>=Counts.x)return;FluidParticle p=Particles[id];if(!p.velocityFlags.w)return;
    float dt=GravityDt.w;
    p.positionRadius.xyz+=p.velocityFlags.xyz*dt+.5*GravityDt.xyz*dt*dt;
    p.velocityFlags.xyz+=GravityDt.xyz*dt;
    float3 lo=DomainMinCell.xyz+p.positionRadius.w,hi=DomainMaxRadius.xyz-p.positionRadius.w;
    [unroll]for(uint axis=0;axis<3;++axis) {
        if(p.positionRadius[axis]<lo[axis]){p.positionRadius[axis]=lo[axis];p.velocityFlags[axis]=max(p.velocityFlags[axis],0);}
        if(p.positionRadius[axis]>hi[axis]){p.positionRadius[axis]=hi[axis];p.velocityFlags[axis]=min(p.velocityFlags[axis],0);}
    }
    Particles[id]=p;
}
