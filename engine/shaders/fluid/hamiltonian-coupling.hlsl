#include "common.hlsli"
#include "hamiltonian-shared.hlsli"
#include "hamiltonian-surface.hlsli"
cbuffer WavePass : register(b4) {uint Dst,A,B,Op;float X,Y;uint Reserved0,Reserved1;};
RWStructuredBuffer<float2> Pool : register(u39);
RWStructuredBuffer<uint> FreeIds : register(u40);
RWStructuredBuffer<uint> Stats : register(u41);
uint emissionHash(uint s){s^=s>>16;s*=0x7feb352d;s^=s>>15;s*=0x846ca68b;return s^(s>>16);}
// Activity is an observer of physical state, independent of the camera/player,
// valve state and rendering. Disconnected wet bodies get independent regions.
[numthreads(64,1,1)]void Activity(uint i:SV_DispatchThreadID) {
    uint n=WaveGrid.x;if(i>=n*n)return;
    float2 xz=WaveDomain.xy+(float2(i%n,i/n)+.5)*WaveDomain.zw/n;
    float height=WaveData[i].x,bottom=WavePhysics.y-WavePhysics.x;
    float h=DomainMinCell.w,band=WaveCoupling.y,body=-1e3;
    float2 spacing=WaveDomain.zw/n;
    for(uint s=0;s<Collision.x;++s) {
        FluidCollider c=Colliders[s];if(uint(c.extentType.w)==4)continue;
        if(uint(c.extentType.w)==6) {
            float terrain=oceanTerrainHeight(xz.x,xz.y);
            float2 slope=float2(oceanTerrainHeight(xz.x+.5,xz.y)-oceanTerrainHeight(xz.x-.5,xz.y),
                                oceanTerrainHeight(xz.x,xz.y+.5)-oceanTerrainHeight(xz.x,xz.y-.5));
            // Promote a coastal band from actual bathymetry. Deep, flat seabed
            // is not treated as a submerged rigid body covering the whole sea.
            if(terrain>.05)body=max(body,2*band+2*h-abs(height-terrain)/max(length(slope),.12));
            continue;
        }
        float3 center=c.centerRestitution.xyz;
        if(uint(c.extentType.w)==5) {
            float3 localCenter=c.meshMinimumSpacing.xyz+.5*float3(c.meshDimensions.xyz-1)*c.meshMinimumSpacing.w;
            center=mul(localCenter-c.worldToLocal[3].xyz,transpose((float3x3)c.worldToLocal));
        }
        float3 p=float3(xz.x,clamp(center.y,bottom,height),xz.y);
        float3 v=c.velocityFriction.xyz;
        float distance=min(colliderPhi(c,p),min(colliderPhi(c,p-v*.15),colliderPhi(c,p+v*.35)));
        // Only a body touching the liquid (or its imminent swept path) can
        // demand refinement. Vertical separation is evaluated by the same SDF.
        float clearance=colliderPhi(c,float3(center.x,clamp(center.y,bottom,height),center.z));
        if(clearance<h)body=max(body,2*band+2*h-distance);
    }
    float2 dh=float2((wavePlane(xz+float2(spacing.x,0),0).x-wavePlane(xz-float2(spacing.x,0),0).x)/(2*spacing.x),
                    (wavePlane(xz+float2(0,spacing.y),0).x-wavePlane(xz-float2(0,spacing.y),0).x)/(2*spacing.y));
    float activity=smoothstep(.18,.35,length(dh));
    if(Op==0) {
        uint3 cell=cellCoord(float3(xz.x,bottom,xz.y));
        float scale=sqrt(WavePhysics.z*max(WavePhysics.x,h));
        for(uint y=0;y<Grid.y;++y) {
            cell.y=y;uint index=cellIndex(cell);
            for(uint j=CellOffsets[index];j<CellOffsets[index+1];++j) {
                FluidParticle p=Particles[SortedIndices[j]];
                float3 curl=float3(p.apic2.y-p.apic1.z,p.apic0.z-p.apic2.x,p.apic1.x-p.apic0.y);
                float residual=length(p.velocityFlags.xyz-waveVelocity(p.positionRadius.xyz));
                activity=max(activity,max(smoothstep(.12,.35,length(curl)*h/scale),smoothstep(.15,.45,residual/scale)));
            }
        }
        activity=max(activity,asfloat(Stats[64+n*n*4+i]));
    }
    Stats[64+n*n*4+i]=0;
    float old=Op!=0?0:WaveData[(WaveGrid.z+1)*n*n+i].z;
    activity=max(activity,old*exp(-WaveCoupling.z/1.0));
    WaveData[(WaveGrid.z+2)*n*n+i]=float4(body,activity,0,0);
}
[numthreads(64,1,1)]void Regions(uint i:SV_DispatchThreadID) {
    uint n=WaveGrid.x;if(i>=n*n)return;int2 q=int2(i%n,i/n);
    float2 spacing=WaveDomain.zw/n;float band=WaveCoupling.y;
    uint raw=(WaveGrid.z+2)*n*n,base=(WaveGrid.z+1)*n*n;
    float requested=WaveData[raw+i].x;
    int2 radius=int2(ceil((2*band+2*DomainMinCell.w)/spacing));
    for(int z=-radius.y;z<=radius.y;++z)for(int x=-radius.x;x<=radius.x;++x) {
        int2 p=q+int2(x,z);if(any(p<0)||any(p>=int(n)))continue;
        if(WaveData[raw+p.y*n+p.x].y>.5)
            requested=max(requested,2*band+2*DomainMinCell.w-length(float2(x,z)*spacing));
    }
    float4 old=WaveData[base+i];
    float distance=Op!=0?requested:max(requested,old.x-.5*WaveCoupling.z);
    float previous=Op!=0?-1e3:old.x;
    WaveData[base+i]=float4(distance,previous,WaveData[raw+i].y,0);
    if(distance>=band)InterlockedAdd(Stats[18],1);
    // Coarea estimate of the open interface length, used only for conservative
    // basin/particle transfer correction, never as an excitation source.
    if(abs(distance-band)<.5*length(spacing))
        InterlockedAdd(Stats[19],uint(round(spacing.x*spacing.y/length(spacing)*1000)));
    if(Op==0&&((distance>=band)!=(previous>=band)))InterlockedAdd(Stats[20],1);
}
void transferWater(float3 p,float3 velocity,int count) {
    uint2 q=uint2(clamp((p.xz-WaveDomain.xy)/WaveDomain.zw*WaveGrid.x,0,float(WaveGrid.x-1)));
    uint base=64+(q.y*WaveGrid.x+q.x)*4;
    InterlockedAdd(Stats[base],uint(count));
    for(uint a=0;a<3;++a)InterlockedAdd(Stats[base+1+a],uint(int(round(velocity[a]*count*10000))));
    InterlockedAdd(Stats[13],uint(count));
}
[numthreads(64,1,1)]void TargetHeight(uint i:SV_DispatchThreadID) {
    uint n=WaveGrid.y;if(i>=n*n)return;uint2 q=uint2(i%n,i/n);q=min(q,n-1-q);
    float2 xz=WaveDomain.xy+(float2(q)+.5)/WaveGrid.x*WaveDomain.zw;
    float2 target=0;
    if(waveInterior(xz)>0) {
        uint3 c=cellCoord(float3(xz.x,DomainMinCell.y,xz.y));
        uint air=0;bool wet=false;float height=DomainMinCell.y;
        // Connected columns reject detached spray after three empty cells.
        // The renderer is particle implicit: use the particle top within the
        // connected column, then smooth in wave space before relaxing eta.
        for(uint y=0;y<Grid.y;++y) {
            c.y=y;uint cell=cellIndex(c),start=CellOffsets[cell],end=CellOffsets[cell+1];
            if(end>start) {
                wet=true;air=0;
                for(uint j=start;j<end;++j)height=max(height,Particles[SortedIndices[j]].positionRadius.y);
            }else if(wet&&++air>=3)break;
        }
        if(wet) {
            float support=1.5*DomainMinCell.w,y=height+support*(63.0/256.0);
            bool resolved=true;
            // The analytic kernel derivative locates the shared implicit
            // surface with bounded Newton steps instead of repeated gathers
            // for a full column ray march. Extrema only initialize the solve.
            for(uint k=0;k<4;++k) {
                float derivative;float phi=waveParticleNode(float3(xz.x,y,xz.y),false,derivative).x;
                if(derivative<.05){resolved=false;break;}
                y=clamp(y-clamp(phi/derivative,-support*.5,support*.5),height-support,height+support);
                if(abs(phi)<.001*DomainMinCell.w)break;
            }
            if(resolved)target=float2(y-WavePhysics.y,1);
        }
    }
    Pool[Dst*n*n+i]=target;
}
[numthreads(64,1,1)]void Cull(uint i:SV_DispatchThreadID) {
    if(i>=Counts.x)return;FluidParticle p=Particles[i];
    if(Op!=0&&p.velocityFlags.w!=0) {
        float height=wavePlane(p.positionRadius.xz,0).x;
        p.positionRadius.y=DomainMinCell.y+(p.positionRadius.y-DomainMinCell.y)*(height-DomainMinCell.y)/WavePhysics.x;
        p.velocityFlags.xyz=waveVelocity(p.positionRadius.xyz);Particles[i]=p;
    }
    float edge=waveEdge(p.positionRadius.xz),oldEdge=wavePreviousEdge(p.positionRadius.xz);
    if(p.velocityFlags.w==1&&edge>=WaveCoupling.y&&(Op!=0||oldEdge>=WaveCoupling.y)) {
        p.apic1.w=1;Particles[i]=p; // this sample has belonged to the pressure-grid interior
    }
    if(p.velocityFlags.w==1&&(edge<WaveCoupling.y||(Op==0&&oldEdge<WaveCoupling.y))) {
        // Initialization can project lattice samples above nearby solids. Those
        // samples are part of constructing the boundary reservoir, not water
        // exported by an advancing pressure solve (reset may also be paused).
        if(Op==0&&p.apic1.w!=0&&p.positionRadius.y>max(wavePlane(p.positionRadius.xz,0).x,WavePhysics.y)+2*DomainMaxRadius.w) {
            // Detached water crossing out of the 3D region remains physical
            // water. Debit the connected basin until it lands again.
            p.velocityFlags.w=2;
            transferWater(p.positionRadius.xyz,0,-1);
            if(p.apic1.w!=0)InterlockedAdd(Stats[14],uint(-1));
        }else {p=(FluidParticle)0;InterlockedAdd(Stats[2],1);}
        Particles[i]=p;
    }
    if(p.velocityFlags.w==0){uint slot;InterlockedAdd(Stats[0],1,slot);FreeIds[slot]=i;}
    else if(p.velocityFlags.w==1)InterlockedAdd(Stats[9],1);
}
[numthreads(64,1,1)]void Emit(uint i:SV_DispatchThreadID) {
    if(i>=Dst)return;
    if(i==0)Stats[16]=Dst;
    uint slot;InterlockedAdd(Stats[1],1,slot);
    if(slot>=Stats[0]-min(Stats[0],Op)){InterlockedAdd(Stats[17],1);return;}
    float3 axis=normalize(EmitterVelocity.xyz);
    float3 u=normalize(cross(axis,abs(axis.y)<.9?float3(0,1,0):float3(1,0,0))),v=cross(axis,u);
    uint hash=emissionHash(B+A+i);
    float radius=sqrt((hash>>8)*(1.0/16777216.0))*EmitterOriginRadius.w;
    float angle=6.28318530718*(emissionHash(hash)>>8)*(1.0/16777216.0);
    float length=Dst*InitialMinimum.w/(3.14159265359*EmitterOriginRadius.w*EmitterOriginRadius.w);
    FluidParticle p=(FluidParticle)0;
    p.positionRadius=float4(EmitterOriginRadius.xyz+radius*(u*cos(angle)+v*sin(angle))+axis*((i+.5)/Dst*length),DomainMaxRadius.w);
    p.velocityFlags=float4(EmitterVelocity.xyz,2);p.apic0.w=1;
    uint id=FreeIds[slot];Particles[id]=p;PreviousPositions[id]=p.positionRadius;
    InterlockedAdd(Stats[12],1);
}
[numthreads(64,1,1)]void FreeWater(uint i:SV_DispatchThreadID) {
    if(i>=Counts.x)return;FluidParticle p=Particles[i];if(p.velocityFlags.w!=2)return;
    float dt=GravityDt.w;
    // CFL-sized motion and SDF contact apply everywhere in the basin, including
    // sources and detached splashes outside the embedded pressure grid.
    uint substeps=max(1u,uint(ceil(length(p.velocityFlags.xyz)*dt/max(.5*p.positionRadius.w,.001))));
    substeps=min(substeps,32u);float h=dt/substeps;
    for(uint step=0;step<substeps;++step) {
        p.positionRadius.xyz+=p.velocityFlags.xyz*h+.5*GravityDt.xyz*h*h;
        p.velocityFlags.xyz+=GravityDt.xyz*h;
        for(uint sweep=0;sweep<2;++sweep)for(uint s=0;s<Collision.x;++s) {
            FluidCollider c=Colliders[s];float phi=colliderPhi(c,p.positionRadius.xyz);
            if(phi>=p.positionRadius.w)continue;
            float3 n;float e=max(.0001,DomainMinCell.w*.01);
            for(uint a=0;a<3;++a){float3 q=0;q[a]=e;n[a]=colliderPhi(c,p.positionRadius.xyz+q)-colliderPhi(c,p.positionRadius.xyz-q);}
            n=dot(n,n)>1e-15?normalize(n):float3(0,1,0);
            p.positionRadius.xyz+=n*(p.positionRadius.w-phi);
            float3 wall=colliderVelocity(c,p.positionRadius.xyz),relative=p.velocityFlags.xyz-wall;
            float vn=dot(relative,n);
            if(vn<0)relative-=n*(1+saturate(c.centerRestitution.w))*vn;
            p.velocityFlags.xyz=wall+lerp(relative,n*dot(relative,n),saturate(1-c.angularSlip.w));
            InterlockedAdd(Stats[15],1);
        }
        float3 lo=float3(WaveDomain.x,DomainMinCell.y,WaveDomain.y)+p.positionRadius.w;
        float3 hi=float3(WaveDomain.xy+WaveDomain.zw,DomainMaxRadius.y).xzy-p.positionRadius.w;
        for(uint a=0;a<3;++a) {
            if(p.positionRadius[a]<lo[a]){p.positionRadius[a]=lo[a];p.velocityFlags[a]=max(p.velocityFlags[a],0);}
            if(p.positionRadius[a]>hi[a]){p.positionRadius[a]=hi[a];p.velocityFlags[a]=min(p.velocityFlags[a],0);}
        }
        float height=wavePlane(p.positionRadius.xz,0).x;
        bool blocked=false;
        for(uint s=0;s<Collision.x&&!blocked;++s)blocked=colliderPhi(Colliders[s],p.positionRadius.xyz)<p.positionRadius.w-1e-5;
        if(!blocked&&p.positionRadius.y-p.positionRadius.w<=height) {
            transferWater(p.positionRadius.xyz,p.velocityFlags.xyz,1);
            if(waveEdge(p.positionRadius.xz)>=WaveCoupling.y) {
                p.velocityFlags.w=1; // hand actual mass and momentum to APIC
                p.apic1.w=1;
                InterlockedAdd(Stats[14],1);
                InterlockedAdd(Stats[9],1);
            }else p=(FluidParticle)0; // wave domain now owns this water
            break;
        }
    }
    if(p.velocityFlags.w==2) {
        InterlockedAdd(Stats[11],1);
        if(WaveMass.w!=0&&p.positionRadius.y-wavePlane(p.positionRadius.xz,0).x<WaveCoupling.y) {
            uint2 q=uint2(clamp((p.positionRadius.xz-WaveDomain.xy)/WaveDomain.zw*WaveGrid.x,0,float(WaveGrid.x-1)));
            InterlockedMax(Stats[64+WaveGrid.x*WaveGrid.x*4+q.y*WaveGrid.x+q.x],asuint(1.0));
        }
    }
    Particles[i]=p;
}
[numthreads(64,1,1)]void Seed(uint i:SV_DispatchThreadID) {
    if(i>=Dst)return;uint3 q=uint3(i%InitialLattice.x,i/(InitialLattice.x*InitialLattice.z),(i/InitialLattice.x)%InitialLattice.z);
    float3 spacing=(InitialMaximum.xyz-InitialMinimum.xyz)/float3(InitialLattice.xyz);
    // A fixed lattice, retired/reseeded each step, cannot cross the FAB edge
    // when velocity*dt is smaller than its distance to that edge. Stratified
    // horizontal phases integrate even tiny inward fluxes without accumulating
    // duplicate boundary samples. The analytic surface hides reservoir jitter.
    uint sx=emissionHash(i^emissionHash(B*2+1)),sz=emissionHash(sx^emissionHash(B*2+2));
    float3 phase=float3((sx>>8)*(1.0/16777216.0),.5,(sz>>8)*(1.0/16777216.0));
    float3 p=InitialMinimum.xyz+(float3(q)+phase)*spacing;
    float edge=waveEdge(p.xz),oldEdge=wavePreviousEdge(p.xz);
    if(p.y>DomainMaxRadius.y-DomainMaxRadius.w)return;
    if(WaveMass.w!=0) {
        if(edge<0||(edge>=WaveCoupling.y&&Op==0&&oldEdge>=WaveCoupling.y))return;
    }else if(edge>=WaveCoupling.y)return;
    float height=wavePlane(p.xz,0).x;
    if(p.y>height-DomainMaxRadius.w)return;
    for(uint s=0;s<Collision.x;++s)if(colliderPhi(Colliders[s],p)<DomainMaxRadius.w)return;
    uint slot;InterlockedAdd(Stats[1],1,slot);
    if(slot>=Stats[0]){InterlockedAdd(Stats[3],1);return;}
    uint id=FreeIds[slot];FluidParticle particle=(FluidParticle)0;
    particle.positionRadius=float4(p,DomainMaxRadius.w);
    particle.velocityFlags=float4(waveBoundaryVelocity(p),1);particle.apic0.w=1;
    Particles[id]=particle;PreviousPositions[id]=particle.positionRadius;
}
