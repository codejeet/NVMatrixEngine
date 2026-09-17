#include "common.hlsli"
#include "work-shared.hlsli"
RWStructuredBuffer<uint> DensityArguments : register(u28);
[numthreads(256,1,1)]void WorkZeroPressure(uint id:SV_DispatchThreadID){
    if(id<Grid.w){PressureIn[id]=0;PressureOut[id]=0;}
}
// Validation aliases the arguments root view to the dense face reference.
// Accumulate across ALL substeps/regular and repair solves; a skipped final
// repair must never hide an earlier numerical difference in a CPU snapshot.
[numthreads(256,1,1)]void WorkCompareFaces(uint id:SV_DispatchThreadID){
    if(id==0)InterlockedAdd(SimulationWork[11],1);
    if(id>=3*faceStride())return;
    float4 a=Faces[id],b=asfloat(uint4(SimulationArguments[id*4],SimulationArguments[id*4+1],
        SimulationArguments[id*4+2],SimulationArguments[id*4+3]));
    if(any(!isfinite(a))||any(!isfinite(b))||any(a!=b))InterlockedOr(SimulationWork[7],1);
}
[numthreads(256,1,1)]void WorkCompareDensity(uint id:SV_DispatchThreadID){
    if(id==0)InterlockedAdd(SimulationWork[12],1);
    if(id>=Grid.w)return;
    float a=PressureIn[id],b=PressureOut[id];
    if(!isfinite(a)||!isfinite(b)||a!=b)InterlockedOr(SimulationWork[8],1);
}
// Prefix: current face/density tile counts, frame face tiles/builds,
// frame density tiles/builds, occupied-bin visits, reserved diagnostics.
[numthreads(256,1,1)]void WorkReset(uint id:SV_DispatchThreadID){
    if(id<workSize())SimulationWork[id]=0;if(id<16)SimulationArguments[id]=0;
}
[numthreads(16,1,1)]void WorkBegin(uint id:SV_DispatchThreadID){if(id>=2)SimulationWork[id]=0;}
[numthreads(256,1,1)]void WorkClearFaces(uint id:SV_DispatchThreadID){
    // Removing an active tile must not retain last substep's velocity/mass.
    if(id<3*faceStride())Faces[id]=0;
    if(id<workFaceCapacity())SimulationWork[workFaceFlags()+id]=0;
    if(id==0)SimulationWork[0]=0;
}
[numthreads(128,1,1)]void WorkMarkFaces(uint id:SV_DispatchThreadID){
    if(id>=Grid.w||!cellOccupied(id))return;
    InterlockedAdd(SimulationWork[6],1);
    int3 cell=int3(cellFromIndex(id));uint3 tiles=workFaceGrid();uint stride=workProduct(tiles);
    // Union of the exact quadratic support for ANY particle in this bin:
    // normal faces c-1..c+2; transverse faces c-1..c+1. Interior ownership
    // contributes through its exact occupied-cell quanta, never visibility.
    [unroll]for(uint axis=0;axis<3;++axis){
        int3 last=int3(Grid.xyz)-1;last[axis]++;
        int3 hi=cell+1;hi[axis]++;hi=min(hi,last);
        uint3 loTile=uint3(max(cell-1,0))/workShape(),hiTile=uint3(hi)/workShape();
        for(uint z=loTile.z;z<=hiTile.z;++z)for(uint y=loTile.y;y<=hiTile.y;++y)for(uint x=loTile.x;x<=hiTile.x;++x)
            InterlockedOr(SimulationWork[workFaceFlags()+axis*stride+workIndex(uint3(x,y,z),tiles)],1);
    }
}
[numthreads(128,1,1)]void WorkCompactFaces(uint id:SV_DispatchThreadID){
    if(id>=workFaceCapacity()||SimulationWork[workFaceFlags()+id]==0)return;
    uint slot;InterlockedAdd(SimulationWork[0],1,slot);SimulationWork[workFaceList()+slot]=id;
}
void workArguments(uint offset,uint count){
    SimulationArguments[offset]=min(count,65535u);
    SimulationArguments[offset+1]=max(1u,(count+65534)/65535);SimulationArguments[offset+2]=1;
}
[numthreads(1,1,1)]void WorkPrepareFaces(){
    uint count=SimulationWork[0];workArguments(0,count);SimulationWork[2]+=count;SimulationWork[3]++;
}
[numthreads(128,1,1)]void WorkClearDensity(uint id:SV_DispatchThreadID){
    if(id<workProduct(workDensityGrid()))SimulationWork[workDensityFlags()+id]=0;
    if(id==0)SimulationWork[1]=0;
}
void workDensity(uint tile,bool repair){
    uint3 g=workDensityGrid();if(tile>=workProduct(g)||(repair&&DensityArguments[24]==0))return;
    uint3 base=workCoordinate(tile,g)*workShape();bool active=false;
    for(uint z=0;z<4&&!active;++z)for(uint y=0;y<4&&!active;++y)for(uint x=0;x<8&&!active;++x){
        uint3 p=base+uint3(x,y,z);if(all(p<Grid.xyz))active=FaceScratch[cellIndex(p)].y>0;
    }
    if(active){uint slot;InterlockedAdd(SimulationWork[1],1,slot);SimulationWork[workDensityList()+slot]=tile;
        SimulationWork[workDensityFlags()+tile]=1;}
}
[numthreads(128,1,1)]void WorkDensity(uint id:SV_DispatchThreadID){workDensity(id,false);}
[numthreads(128,1,1)]void WorkDensityRepair(uint id:SV_DispatchThreadID){workDensity(id,true);}
[numthreads(1,1,1)]void WorkPrepareDensity(){
    uint count=SimulationWork[1];workArguments(3,count);SimulationWork[4]+=count;SimulationWork[5]++;
}
