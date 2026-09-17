// A quantity has exactly one owner. FP32 particles are caches; authoritative
// mass and linear momentum remain FP64 across retire/restore transactions.
cbuffer ExchangeFrame:register(b0){
    uint4 Fine,Coarse,Work; // particle capacity, first birth, birth count, site stride
    float4 Physical; // particle volume, radius, reserved, reserved
    uint4 Allocation; // reusable particle prefix (never steal future emitter IDs)
}
struct Particle{float4 positionRadius,velocityFlags,apic0,apic1,apic2;};
RWStructuredBuffer<Particle> Particles:register(u0);
RWStructuredBuffer<double4> ParticleQuantity:register(u1);
RWStructuredBuffer<float4> ReferenceVelocity:register(u2);
RWStructuredBuffer<double4> GridQuantity:register(u3);
RWStructuredBuffer<uint> Offsets:register(u4);
RWStructuredBuffer<uint> Indices:register(u5);
RWStructuredBuffer<uint> Requests:register(u6); // 1 grid, 0 particles
RWStructuredBuffer<double2> Capacity:register(u7);
RWStructuredBuffer<float4> Sites:register(u8); // caller-validated reconstruction sites
RWStructuredBuffer<uint> SiteCounts:register(u9);
RWStructuredBuffer<uint> FreeIds:register(u10);
RWStructuredBuffer<uint> Control:register(u11);
RWStructuredBuffer<float4> PreviousPositions:register(u12);
// [0] free, [1] reserved, [2] retired, [3] restored, [4] deposit cells,
// [5] restore cells, [6] deferred cells, [7] invalid, [8] capacity refusals.
// [16] cumulative invalid since reset, not hidden by per-frame counter clearing.
void invalidExchange(){InterlockedAdd(Control[7],1);InterlockedAdd(Control[16],1);}
uint index(uint3 p,uint3 n){return (p.z*n.y+p.y)*n.x+p.x;}
uint3 coord(uint id,uint3 n){return uint3(id%n.x,(id/n.x)%n.y,id/(n.x*n.y));}
bool finite64(double4 q){uint4 lo,hi;asuint(q,lo,hi);return all((hi&0x7ff00000u)!=0x7ff00000u);}
bool finiteQuantity(double4 q){return finite64(q)&&q.w>=0&&(q.w>0||all(q.xyz==0));}
[numthreads(128,1,1)]void ExchangeReset(uint id:SV_DispatchThreadID){
    if(id<Work.x){ParticleQuantity[id]=0;ReferenceVelocity[id]=0;}
    if(id<Coarse.w)GridQuantity[id]=0;
    if(id==0)Control[16]=0;
}
[numthreads(16,1,1)]void ExchangeClear(uint id:SV_DispatchThreadID){Control[id]=0;}
[numthreads(128,1,1)]void ExchangeSeed(uint id:SV_DispatchThreadID){
    if(id>=Work.z)return;id+=Work.y;
    Particle p=Particles[id];double4 q=double4(p.velocityFlags.xyz,1)*(double(Physical.x)*double(p.apic0.w));
    if(!p.velocityFlags.w||ParticleQuantity[id].w!=0||!finiteQuantity(q)||q.w==0){invalidExchange();return;}
    ParticleQuantity[id]=q;ReferenceVelocity[id]=float4(p.velocityFlags.xyz,0);PreviousPositions[id]=p.positionRadius;
}
[numthreads(128,1,1)]void ExchangeVelocityDelta(uint id:SV_DispatchThreadID){
    if(id>=Work.x)return;
    if(!Particles[id].velocityFlags.w){if(any(ParticleQuantity[id]!=0))invalidExchange();return;}
    double4 q=ParticleQuantity[id];float3 v=Particles[id].velocityFlags.xyz;
    double3 delta=double3(v)-double3(ReferenceVelocity[id].xyz);
    q.xyz+=q.w*delta;
    if(!finiteQuantity(q)||q.w==0){invalidExchange();return;}
    ParticleQuantity[id]=q;ReferenceVelocity[id]=float4(v,0);
}
[numthreads(64,1,1)]void ExchangeDeposit(uint id:SV_DispatchThreadID){
    if(id>=Coarse.w||Requests[id]!=1)return;
    if(!finiteQuantity(GridQuantity[id])||!finite64(Capacity[id].xxxx)||Capacity[id].x<0){invalidExchange();return;}
    uint3 base=coord(id,Coarse.xyz)*2;double4 added=0;uint count=0;
    // Current, disjoint fine-cell ranges give each live particle one writer.
    [unroll]for(uint child=0;child<8;child++){
        uint3 p=base+uint3(child&1,(child>>1)&1,child>>2);if(any(p>=Fine.xyz))continue;
        uint cell=index(p,Fine.xyz);
        for(uint j=Offsets[cell];j<Offsets[cell+1];j++){
            uint pid=Indices[j];
            if(pid>=Work.x||!Particles[pid].velocityFlags.w||!finiteQuantity(ParticleQuantity[pid])||ParticleQuantity[pid].w<=0){invalidExchange();return;}
            added+=ParticleQuantity[pid];count++;
        }
    }
    if(!count)return;
    double4 next=GridQuantity[id]+added;
    if(!finiteQuantity(next)){invalidExchange();return;}
    if(next.w>Capacity[id].x){InterlockedAdd(Control[8],1);return;}
    GridQuantity[id]=next;
    [unroll]for(uint child=0;child<8;child++){
        uint3 p=base+uint3(child&1,(child>>1)&1,child>>2);if(any(p>=Fine.xyz))continue;
        uint cell=index(p,Fine.xyz);
        for(uint j=Offsets[cell];j<Offsets[cell+1];j++){
            uint pid=Indices[j];Particles[pid]=(Particle)0;ParticleQuantity[pid]=0;ReferenceVelocity[pid]=0;
        }
    }
    InterlockedAdd(Control[2],count);InterlockedAdd(Control[4],1);
}
[numthreads(128,1,1)]void ExchangeFree(uint id:SV_DispatchThreadID){
    if(id>=Allocation.x||Particles[id].velocityFlags.w)return;
    if(any(ParticleQuantity[id]!=0)){invalidExchange();return;}
    uint slot;InterlockedAdd(Control[0],1,slot);FreeIds[slot]=id;
}
// CAS only reserves successful transactions. Failed large requests do not
// consume IDs needed by smaller cells, nor can counters wrap past capacity.
bool reserveIds(uint count,out uint first){
    uint available=Control[0],old=Control[1];first=0;
    for(;;){
        if(old>available||count>available-old)return false;
        uint actual;InterlockedCompareExchange(Control[1],old,old+count,actual);
        if(actual==old){first=old;return true;}old=actual;
    }
}
[numthreads(64,1,1)]void ExchangeRestore(uint id:SV_DispatchThreadID){
    if(id>=Coarse.w||Requests[id]!=0)return;
    double4 q=GridQuantity[id];if(!finiteQuantity(q)){invalidExchange();return;}if(q.w==0)return;
    uint count=SiteCounts[id];
    if(count>Work.w){invalidExchange();return;}
    if(!count){InterlockedAdd(Control[6],1);return;}
    double4 each=q/double(count);
    float mass=float(each.w/double(Physical.x));float3 velocity=float3(q.xyz/q.w);
    // Validate everything before reserving IDs or changing either owner.
    if(!(mass>0)||!isfinite(mass)||any(!isfinite(velocity))){invalidExchange();return;}
    for(uint i=0;i<count;i++)if(any(!isfinite(Sites[id*Work.w+i].xyz))){invalidExchange();return;}
    uint first;if(!reserveIds(count,first)){InterlockedAdd(Control[6],1);return;}
    double4 remaining=q;
    for(uint i=0;i<count;i++){
        uint pid=FreeIds[first+i];double4 owned=i+1==count?remaining:each;remaining-=owned;
        Particle p=(Particle)0;p.positionRadius=float4(Sites[id*Work.w+i].xyz,Physical.y);
        p.velocityFlags=float4(velocity,1);p.apic0.w=float(owned.w/double(Physical.x));
        Particles[pid]=p;ParticleQuantity[pid]=owned;ReferenceVelocity[pid]=float4(velocity,0);
        PreviousPositions[pid]=p.positionRadius;
    }
    GridQuantity[id]=0;InterlockedAdd(Control[3],count);InterlockedAdd(Control[5],1);
}
