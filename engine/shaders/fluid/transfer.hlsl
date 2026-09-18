#include "common.hlsli"
#if FLUID_HAMILTONIAN
#include "hamiltonian-shared.hlsli"
#endif
#include "work-shared.hlsli"
#if FLUID_SPACETIME
#include "spacetime-shared.hlsli"
#endif
void gatherFace(uint id) {
    uint stride=faceStride();if(id>=stride*3)return;
    uint axis=id/stride,k=id%stride;
    uint3 cell=uint3(k%(Grid.x+1),(k/(Grid.x+1))%(Grid.y+1),k/((Grid.x+1)*(Grid.y+1)));
    uint3 extent=Grid.xyz;extent[axis]++;
    if(any(cell>=extent)){Faces[id]=0;return;}
#if FLUID_HAMILTONIAN
    float3 world=DomainMinCell.xyz+(float3(cell)+faceOffset(axis))*DomainMinCell.w;
    if(WaveMass.w!=0&&waveEdge(world.xz)<-1.5*DomainMinCell.w){Faces[id]=0;return;}
#endif
    float3 gridPosition=float3(cell)+faceOffset(axis);
    int3 lo=int3(floor(gridPosition-1.5)),hi=int3(ceil(gridPosition+1.5))-1;
    lo=max(lo,0);hi=min(hi,int3(Grid.xyz)-1);
    float momentum=0,mass=0;
    // Face gather via compact cell ranges. No contended float CAS and no
    // per-frame CPU work. The fixed support visits only neighboring bins.
    for(int z=lo.z;z<=hi.z;++z)for(int y=lo.y;y<=hi.y;++y)for(int x=lo.x;x<=hi.x;++x) {
        uint c=cellIndex(uint3(x,y,z)),end=CellOffsets[c+1];
        for(uint j=CellOffsets[c];j<end;++j) {
            uint pid=SortedIndices[j];FluidParticle p=Particles[pid];
            // Match G2P's grid-relative coordinates: avoid subtracting large
            // world positions differently in the two transfer directions.
            float3 q=(p.positionRadius.xyz-DomainMinCell.xyz)/DomainMinCell.w-gridPosition;
            float massWeight=particleWeight(p);float particleVelocity=p.velocityFlags[axis];
#if FLUID_PARTICLE_AUTHORITY
            double4 physical=ParticleQuantity[pid];
            massWeight=float(physical.w/double(InitialMinimum.w));
            particleVelocity=physical.w>0?float(physical[axis]/physical.w):0;
#endif
            float w=quadratic(q.x)*quadratic(q.y)*quadratic(q.z)*massWeight;
#if FLUID_SPACETIME
            // Weight mass and the full APIC momentum contribution identically.
            w*=temporalParticleWeight(pid);
#endif
            float3 row=axis==0?p.apic0.xyz:(axis==1?p.apic1.xyz:p.apic2.xyz);
            if(Solver.z>0)row=0; // FLIP/PIC and APIC are distinct transfer schemes.
            momentum+=w*(particleVelocity-dot(row,q*DomainMinCell.w));mass+=w;
        }
    }
    float velocity=mass>1e-8?momentum/mass:0;
#if !FLUID_SPACETIME
    if(Display.z){float2 bulk=interiorGather(gridPosition,DomainMinCell,Grid.xyz,axis);momentum+=bulk.x;mass+=bulk.y;
        velocity=mass>1e-8?momentum/mass:0;}
#endif
    Faces[id]=float4(velocity,velocity,mass,0);
}
[numthreads(128,1,1)]void P2G(uint3 tid:SV_DispatchThreadID){gatherFace(tid.x);}
[numthreads(128,1,1)]void P2GSparse(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    uint work=workGroup(group);if(work>=SimulationWork[0])return;
    uint tile=SimulationWork[workFaceList()+work],stride=workProduct(workFaceGrid());
    uint axis=tile/stride;uint3 p=workCoordinate(tile%stride,workFaceGrid())*workShape()+uint3(lane%8,(lane/8)%4,lane/32);
    uint3 extent=Grid.xyz;extent[axis]++;if(any(p>=extent))return;
    gatherFace(faceIndex(p,axis));
}

// Extrapolate projected and pre-force velocity together so FLIP never samples
// a fabricated zero previous velocity at newly valid free-surface faces.
[numthreads(128,1,1)]
void Extrapolate(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x,stride=faceStride();if(id>=stride*3)return;
    uint axis=id/stride,k=id%stride;
    int3 c=int3(k%(Grid.x+1),(k/(Grid.x+1))%(Grid.y+1),k/((Grid.x+1)*(Grid.y+1)));
    int3 extent=int3(Grid.xyz);extent[axis]++;
    float4 f=Faces[id];
    if(f.w>0||any(c>=extent)){FaceScratch[id]=f;return;}
    float2 sum=0;float count=0;
    [unroll]for(uint a=0;a<3;++a)[unroll]for(int side=-1;side<=1;side+=2) {
        int3 n=c;n[a]+=side;if(any(n<0)||any(n>=extent))continue;
        float4 neighbor=Faces[faceIndex(n,axis)];
        if(neighbor.w>0){sum+=neighbor.xy;count++;}
    }
    if(count>0){f.xy=sum/count;f.w=1;}
    FaceScratch[id]=f;
}

[numthreads(128,1,1)]
void G2P(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x;if(id>=Counts.x)return;
    FluidParticle p=Particles[id];if(p.velocityFlags.w!=1)return;
    float3 pic=0,delta=0,rows[3];float h=DomainMinCell.w;
    for(uint axis=0;axis<3;++axis) {
        float3 gp=(p.positionRadius.xyz-DomainMinCell.xyz)/h-faceOffset(axis);
        int3 base=int3(floor(gp-.5));float3 moment=0;
        for(int z=0;z<3;++z)for(int y=0;y<3;++y)for(int x=0;x<3;++x) {
            int3 cell=base+int3(x,y,z);
            float3 q=float3(cell)-gp;
            float w=quadratic(q.x)*quadratic(q.y)*quadratic(q.z);
            // Constant extension at the domain edge. Normal wall-face velocity
            // remains zero; collision projection removes inward particle motion.
            int3 last=int3(Grid.xyz)-1;last[axis]++;
            float4 f=Faces[faceIndex(clamp(cell,0,last),axis)];
            pic[axis]+=w*f.x;delta[axis]+=w*(f.x-f.y);
            // Partition-of-unity gives sum(w*q)=0. Subtracting a constant
            // avoids cancellation of large bulk velocity in the tiny moment.
            moment+=w*(f.x-p.velocityFlags[axis])*q;
        }
        rows[axis]=moment*(4/h); // inverse quadratic second moment: 4/h^2
    }
    p.velocityFlags.xyz=Solver.z>0?lerp(pic,p.velocityFlags.xyz+delta,Solver.y):pic;
    p.apic0.xyz=rows[0];p.apic1.xyz=rows[1];p.apic2.xyz=rows[2];
    if(Counts.w!=4) {
        // PIC/grid advection also in FLIP mode: do not advect with unresolved
        // particle null-space noise (Ding/Shinar/Schroeder MAC APIC, section 1).
        p.positionRadius.xyz+=pic*GravityDt.w;
        float3 lo=DomainMinCell.xyz+p.positionRadius.w,hi=DomainMaxRadius.xyz-p.positionRadius.w;
        [unroll]for(uint a=0;a<3;++a) {
            if(p.positionRadius[a]<lo[a]){p.positionRadius[a]=lo[a];p.velocityFlags[a]=max(p.velocityFlags[a],0);}
            if(p.positionRadius[a]>hi[a]){p.positionRadius[a]=hi[a];p.velocityFlags[a]=min(p.velocityFlags[a],0);}
        }
    }
    Particles[id]=p;
}
