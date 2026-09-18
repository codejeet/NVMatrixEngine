// One particle surface for coupling and rendering. The radius is the exact
// centroid offset of a uniform half-space under (1-r^2/support^2)^3: 63/256
// of the support. A flat rest lattice therefore matches the wave height,
// rather than adding a kernel-dependent ridge around every 3D region.
#ifndef WAVE_PARTICLE_MINIMUM
#define WAVE_PARTICLE_MINIMUM DomainMinCell
#define WAVE_PARTICLE_GRID Grid.xyz
#define WAVE_PARTICLE_OFFSETS CellOffsets
#define WAVE_PARTICLE_INDICES SortedIndices
#define WAVE_PARTICLE_PREVIOUS PreviousPositions
#endif
float4 waveParticleNode(float3 p,bool withMotion,out float verticalDerivative) {
    float4 minimum=WAVE_PARTICLE_MINIMUM;
    float support=1.5*minimum.w,radius=support*(63.0/256.0);
    int3 a=max(0,int3(floor((p-support-minimum.xyz)/minimum.w)));
    int3 b=min(int3(WAVE_PARTICLE_GRID)-1,int3(floor((p+support-minimum.xyz)/minimum.w)));
    float sum=0,derivativeSum=0;float3 offset=0,motion=0,derivativeOffset=0;
    for(int z=a.z;z<=b.z;++z)for(int y=a.y;y<=b.y;++y)for(int x=a.x;x<=b.x;++x) {
        uint cell=(z*WAVE_PARTICLE_GRID.y+y)*WAVE_PARTICLE_GRID.x+x;
        for(uint j=WAVE_PARTICLE_OFFSETS[cell];j<WAVE_PARTICLE_OFFSETS[cell+1];++j) {
            uint id=WAVE_PARTICLE_INDICES[j];float3 q=Particles[id].positionRadius.xyz-p;
            float k=max(0,1-dot(q,q)/(support*support)),mass=Particles[id].apic0.w;
            float w=k*k*k*mass,dw=6*q.y/(support*support)*k*k*mass;
            sum+=w;offset+=q*w;
            derivativeSum+=dw;derivativeOffset+=q*dw;
            if(withMotion)motion+=(WAVE_PARTICLE_PREVIOUS[id].xyz-Particles[id].positionRadius.xyz)*w;
        }
    }
    float3 center=sum>1e-8?offset/sum:0;
    float3 dc=sum>1e-8?(derivativeOffset-center*derivativeSum)/sum-float3(0,1,0):0;
    verticalDerivative=dot(center,dc)/max(length(center),1e-8);
    return float4(sum>1e-8?length(center)-radius:support,sum>1e-8?motion/sum:0);
}
float4 waveParticleNode(float3 p,bool withMotion) {
    float derivative;return waveParticleNode(p,withMotion,derivative);
}
