#include "common.hlsli"
#include "solid.hlsli"
cbuffer ResampleFrame : register(b1) { uint4 ResampleBricks; uint4 ResampleControl; };
struct ImportanceBrick {float4 importance,dynamics,velocity;uint4 state;};
RWStructuredBuffer<ImportanceBrick> Importance : register(u16);
RWStructuredBuffer<uint> FreeSlots : register(u17);
RWStructuredBuffer<uint> ResampleCounts : register(u18);
RWStructuredBuffer<uint2> Selected : register(u19);
RWByteAddressBuffer ResampleArguments : register(u20);
RWByteAddressBuffer ImportanceArguments : register(u21);
// Consume prior weighted-particle totals before clearing this frame's counters.
// Emitted particles have mass 1. Importance arguments contain groups per LOD.
[numthreads(1,1,1)]void ResamplePlan(){
    bool coarse=(ResampleControl.y&1u)&&!(ResampleControl.y&2u)&&(ImportanceArguments.Load(12)+ImportanceArguments.Load(24)+ImportanceArguments.Load(36)>0);
    bool weighted=ResampleControl.z&&ResampleCounts[7]>ResampleCounts[6]*16;
    uint cellGroups=(Grid.w+63)/64,particleGroups=(Counts.x+255)/256;
    [unroll]for(uint i=0;i<4;++i){uint groups=(ResampleControl.y&4u)&&i>=2?1:(i==2?particleGroups:cellGroups);
        ResampleArguments.Store3(72+i*12,uint3(coarse||weighted?groups:0,1,1));}
}
[numthreads(1,1,1)]void ResampleRebin(){
    bool changed=ResampleCounts[2]+ResampleCounts[3]>0;
    uint cells=(Grid.w+255)/256,particles=(Counts.x+255)/256;
    uint groups[6]={cells,particles,cells,1,cells,particles};
    [unroll]for(uint i=0;i<6;++i)ResampleArguments.Store3(i*12,uint3(changed?groups[i]:0,1,1));
    ResampleCounts[13]=changed?1:0;
}
// Counters: free slots, tickets, merged, split, rejected, starved, samples,
// mass quanta, actual samples by demanded LOD[4], protected cells, reserved.
uint requestedLod(uint cell) {
    if(!(ResampleControl.y&1u)||(ResampleControl.y&2u))return 0;
    uint3 b=(cellFromIndex(cell)+2)/4;
    return Importance[(b.z*ResampleBricks.y+b.y)*ResampleBricks.x+b.x].state.x;
}
// Decisions use current occupancy and collider transforms, even with a frozen
// importance overlay. Do not coarsen within two cells of a newly exposed surface.
uint safeLod(uint cell) {
    uint lod=requestedLod(cell);if(!lod)return 0;
    int3 c=cellFromIndex(cell);
    if(SolidGrid[cell].x<2.5*DomainMinCell.w)return 0;
    [loop]for(int z=-2;z<=2;++z)[loop]for(int y=-2;y<=2;++y)[loop]for(int x=-2;x<=2;++x){
        int3 q=c+int3(x,y,z);
        if(!inGrid(q))return 0;
        uint i=cellIndex(q);if(CellQuanta[i]==0||SolidGrid[i].x<=0)return 0;
    }
    return lod;
}
float3 affineVelocity(FluidParticle p,float3 d){return float3(dot(p.apic0.xyz,d),dot(p.apic1.xyz,d),dot(p.apic2.xyz,d));}
float energy(FluidParticle p){float D=Solver.z>0?0:.25*DomainMinCell.w*DomainMinCell.w;
    return .5*particleWeight(p)*(dot(p.velocityFlags.xyz,p.velocityFlags.xyz)+D*(dot(p.apic0.xyz,p.apic0.xyz)+dot(p.apic1.xyz,p.apic1.xyz)+dot(p.apic2.xyz,p.apic2.xyz)));}
bool clearPosition(float3 p,float radius){
    if(any(p<DomainMinCell.xyz+radius)||any(p>DomainMaxRadius.xyz-radius))return false;
    for(uint i=0;i<Collision.x;++i)if(colliderPhi(Colliders[i],p,radius)<radius)return false;
    return true;
}
[numthreads(16,1,1)]void ResampleClear(uint id:SV_DispatchThreadID){ResampleCounts[id]=0;}
[numthreads(64,1,1)]void ResampleMerge(uint cell:SV_DispatchThreadID){
    if(cell>=Grid.w)return;uint lod=safeLod(cell);
    Selected[cell]=uint2(0xffffffff,lod);
    uint start=CellOffsets[cell],end=CellOffsets[cell+1],population=end-start;
    if(!population)return;
    if(!lod){InterlockedAdd(ResampleCounts[12],1);return;}
    if(population<=max(2u,16u>>lod))return;
    // ID ordering is independent of the atomic scatter order. At most one pair
    // per cell/frame; never empty a cell or collapse an entire interior at once.
    uint a=0xffffffff,b=0xffffffff;float target=float(1u<<lod),smallest=target;
    for(uint j=start;j<end;++j){uint id=SortedIndices[j];float m=particleWeight(Particles[id]);
        if(m<smallest||(m==smallest&&m<target&&id<a)){smallest=m;a=id;}}
    if(a==0xffffffff)return;FluidParticle p=Particles[a];float distance2=.25*DomainMinCell.w*DomainMinCell.w;
    for(uint j=start;j<end;++j){uint id=SortedIndices[j];if(id==a)continue;FluidParticle q=Particles[id];
        float3 d=q.positionRadius.xyz-p.positionRadius.xyz;float d2=dot(d,d);
        if(particleWeight(q)==particleWeight(p)&&(d2<distance2||(d2==distance2&&id<b))){distance2=d2;b=id;}}
    if(b==0xffffffff)return;FluidParticle q=Particles[b],r=p;
    float3 dx=.5*(q.positionRadius.xyz-p.positionRadius.xyz),dv=.5*(q.velocityFlags.xyz-p.velocityFlags.xyz);
    // FLIP does not transfer affine momentum. Reject any collapse that would
    // hide orbital angular momentum in a matrix the next P2G ignores.
    if(Solver.z>0&&length(cross(dx,dv))>1e-9){InterlockedAdd(ResampleCounts[4],1);return;}
    // Cap interpolation error, not absolute velocity: uniform translations merge.
    if(length(dv)>.05||length(affineVelocity(p,dx)-affineVelocity(q,dx))>.05){InterlockedAdd(ResampleCounts[4],1);return;}
    float D=.25*DomainMinCell.w*DomainMinCell.w;
    r.positionRadius.xyz=p.positionRadius.xyz+dx;
    r.velocityFlags.xyz=p.velocityFlags.xyz+dv;
    r.apic0.xyz=.5*(p.apic0.xyz+q.apic0.xyz)+dv.x*dx/D;
    r.apic1.xyz=.5*(p.apic1.xyz+q.apic1.xyz)+dv.y*dx/D;
    r.apic2.xyz=.5*(p.apic2.xyz+q.apic2.xyz)+dv.z*dx/D;
    r.apic0.w=2*particleWeight(p);
    float before=energy(p)+energy(q);
    if(!isfinite(energy(r))||energy(r)>before+max(1e-9,before*2e-6)||!clearPosition(r.positionRadius.xyz,r.positionRadius.w)){
        InterlockedAdd(ResampleCounts[4],1);return;}
    Particles[a]=r;Particles[b]=(FluidParticle)0;
    PreviousPositions[a].xyz=.5*(PreviousPositions[a].xyz+PreviousPositions[b].xyz);
    InterlockedAdd(ResampleCounts[2],1);
}
// Select all parents before any child writes. Reusing a just-freed ID still in
// the old bins during selection would otherwise cause cross-cell data races.
[numthreads(64,1,1)]void ResampleSelect(uint cell:SV_DispatchThreadID){
    if(cell>=Grid.w)return;uint2 choice=Selected[cell];float target=float(1u<<choice.y);
    uint parent=0xffffffff;
    for(uint j=CellOffsets[cell];j<CellOffsets[cell+1];++j){uint id=SortedIndices[j];FluidParticle p=Particles[id];
        if(p.velocityFlags.w&&particleWeight(p)>target)parent=min(parent,id);}
    Selected[cell].x=parent;
}
[numthreads(256,1,1)]void ResampleFree(uint id:SV_DispatchThreadID){
    // Only recycle previously issued slots. Future contiguous emitter ranges
    // remain reserved. A later source allocator can lift this capacity limit.
    if(id>=ResampleControl.x||Particles[id].velocityFlags.w)return;
    uint slot;InterlockedAdd(ResampleCounts[0],1,slot);FreeSlots[slot]=id;
}
[numthreads(1,1,1)]void ResampleFreeOrdered(){
    uint count=0;
    for(uint id=0;id<ResampleControl.x;++id)if(!Particles[id].velocityFlags.w)FreeSlots[count++]=id;
    ResampleCounts[0]=count;
}
void splitSample(uint cell){
    if(cell>=Grid.w)return;uint parent=Selected[cell].x;if(parent==0xffffffff)return;
    FluidParticle p=Particles[parent];float3 x=p.positionRadius.xyz;
    float3 a=DomainMinCell.xyz+float3(cellFromIndex(cell))*DomainMinCell.w;
    float3 room=min(x-a,a+DomainMinCell.w-x);uint axis=room.x>room.y?0:1;if(room.z>room[axis])axis=2;
    float3 d=0;d[axis]=min(.1*DomainMinCell.w,.45*room[axis]);
    if(d[axis]<1e-5*DomainMinCell.w)return;
    if(!clearPosition(x+d,p.positionRadius.w)||!clearPosition(x-d,p.positionRadius.w))return;
    uint ticket;InterlockedAdd(ResampleCounts[1],1,ticket);
    if(ticket>=ResampleCounts[0]){InterlockedAdd(ResampleCounts[5],1);return;}
    uint child=FreeSlots[ticket];FluidParticle q=p;
    float D=.25*DomainMinCell.w*DomainMinCell.w,denominator=D+dot(d,d);
    if(Solver.z==0){p.apic0.xyz-=dot(p.apic0.xyz,d)*d/denominator;
        p.apic1.xyz-=dot(p.apic1.xyz,d)*d/denominator;
        p.apic2.xyz-=dot(p.apic2.xyz,d)*d/denominator;}p.apic0.w*=.5;
    q.apic0=p.apic0;q.apic1=p.apic1;q.apic2=p.apic2;
    float3 dv=Solver.z>0?0:affineVelocity(p,d);
    p.positionRadius.xyz=x-d;q.positionRadius.xyz=x+d;
    p.velocityFlags.xyz-=dv;q.velocityFlags.xyz+=dv;
    Particles[parent]=p;Particles[child]=q;
    float4 old=PreviousPositions[parent];PreviousPositions[parent]=float4(old.xyz-d,old.w);
    PreviousPositions[child]=float4(old.xyz+d,old.w);
    InterlockedAdd(ResampleCounts[3],1);
}
[numthreads(64,1,1)]void ResampleSplit(uint cell:SV_DispatchThreadID){splitSample(cell);}
[numthreads(1,1,1)]void ResampleSplitOrdered(){
    for(uint cell=0;cell<Grid.w;++cell)splitSample(cell);
}
groupshared uint LocalCounts[6];
[numthreads(256,1,1)]void ResampleCount(uint id:SV_DispatchThreadID,uint lane:SV_GroupIndex){
    if(id==0)ResampleCounts[14]=ResampleControl.w?InteriorTotals[1]:0;
    if(lane<6)LocalCounts[lane]=0;GroupMemoryBarrierWithGroupSync();
    if(id<Counts.x){FluidParticle p=Particles[id];if(p.velocityFlags.w){
        InterlockedAdd(LocalCounts[0],1);InterlockedAdd(LocalCounts[1],uint(round(16*particleWeight(p))));
        uint lod=requestedLod(cellIndex(cellCoord(p.positionRadius.xyz)));
        InterlockedAdd(LocalCounts[2+lod],1);}}
    GroupMemoryBarrierWithGroupSync();
    if(lane<6)InterlockedAdd(ResampleCounts[6+lane],LocalCounts[lane]);
}
