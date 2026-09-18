#include "common.hlsli"
groupshared uint Shared[256];
[numthreads(256,1,1)]
void ClearBins(uint3 tid:SV_DispatchThreadID) {
    if(tid.x<Grid.w){CellCounts[tid.x]=0;CellCursor[tid.x]=0;
        if(Display.w)CellQuanta[tid.x]=Display.z?interiorCellQuanta(cellFromIndex(tid.x),Grid.xyz,DomainMinCell):0;}
}
[numthreads(256,1,1)]
void CountBins(uint3 tid:SV_DispatchThreadID) {
    if(tid.x>=Counts.x)return;FluidParticle p=Particles[tid.x];
    if(p.velocityFlags.w==1){uint cell=cellIndex(cellCoord(p.positionRadius.xyz));InterlockedAdd(CellCounts[cell],1);
        if(Display.w==1)InterlockedAdd(CellQuanta[cell],uint(round(16*particleWeight(p))));}
}
#if FLUID_PARTICLE_AUTHORITY
// Run after scatter/sort. FP64 gather avoids atomics and rounds once per cell;
// bin membership remains particle count, independently of physical rest mass.
[numthreads(256,1,1)]void GatherMass(uint id:SV_DispatchThreadID){
    if(id>=Grid.w)return;double volume=0;
    for(uint j=CellOffsets[id];j<CellOffsets[id+1];j++)volume+=ParticleQuantity[SortedIndices[j]].w;
    CellQuanta[id]=asuint(float(volume/double(InitialMinimum.w)));
}
#endif
[numthreads(256,1,1)]
void ScanCells(uint3 tid:SV_DispatchThreadID,uint lane:SV_GroupIndex,uint3 group:SV_GroupID) {
    uint count=tid.x<Grid.w?CellCounts[tid.x]:0;Shared[lane]=count;
    GroupMemoryBarrierWithGroupSync();
    for(uint offset=1;offset<256;offset*=2) {
        uint value=lane>=offset?Shared[lane-offset]:0;
        GroupMemoryBarrierWithGroupSync();Shared[lane]+=value;GroupMemoryBarrierWithGroupSync();
    }
    if(tid.x<Grid.w)CellOffsets[tid.x]=Shared[lane]-count;
    if(lane==255)ScanBlocks[group.x]=Shared[255];
}
[numthreads(1,1,1)]
void ScanSums() {
    // Small top-level scan (414 blocks in the default domain). Replace with a
    // recursive scan for very large domains; no CPU prefix sum or synchronization.
    uint sum=0,blocks=(Grid.w+255)/256;
    for(uint i=0;i<blocks;++i){uint count=ScanBlocks[i];ScanBlocks[i]=sum;sum+=count;}
    CellOffsets[Grid.w]=sum;
}
[numthreads(256,1,1)]
void FinishScan(uint3 tid:SV_DispatchThreadID) {
    if(tid.x<Grid.w)CellOffsets[tid.x]+=ScanBlocks[tid.x/256];
}
[numthreads(256,1,1)]
void Scatter(uint3 tid:SV_DispatchThreadID) {
    if(tid.x>=Counts.x)return;FluidParticle p=Particles[tid.x];if(p.velocityFlags.w!=1)return;
    uint cell=cellIndex(cellCoord(p.positionRadius.xyz)),offset;InterlockedAdd(CellCursor[cell],1,offset);
    SortedIndices[CellOffsets[cell]+offset]=tid.x;
}
// Optional deterministic reference mode: fix the floating-point gather order
// after atomic scatter. Per-cell heapsort needs no extra buffers and has bounded
// O(n log n) work even for unusually crowded cells; normal play skips this pass.
void siftBin(uint start,uint root,uint count){
    uint value=SortedIndices[start+root];
    [loop]while(root<count/2){
        uint child=2*root+1;
        if(child+1<count&&SortedIndices[start+child]<SortedIndices[start+child+1])child++;
        uint next=SortedIndices[start+child];if(value>=next)break;
        SortedIndices[start+root]=next;root=child;
    }
    SortedIndices[start+root]=value;
}
[numthreads(256,1,1)]
void SortBins(uint3 tid:SV_DispatchThreadID){
    if(tid.x>=Grid.w)return;uint start=CellOffsets[tid.x],count=CellOffsets[tid.x+1]-start;
    [loop]for(uint i=count/2;i>0;--i)siftBin(start,i-1,count);
    [loop]for(uint n=count;n>1;--n){
        uint value=SortedIndices[start];SortedIndices[start]=SortedIndices[start+n-1];
        SortedIndices[start+n-1]=value;siftBin(start,0,n-1);
    }
}
