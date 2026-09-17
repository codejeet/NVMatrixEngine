#ifndef FLUID_WORK_SHARED
#define FLUID_WORK_SHARED
// GPU work scheduling over persistent dense transfer storage. This is NOT
// sparse grid allocation and does not remove the global pressure coupling.
RWStructuredBuffer<uint> SimulationWork : register(u29);
RWStructuredBuffer<uint> SimulationArguments : register(u30);
uint3 workShape(){return uint3(8,4,4);}
uint3 workFaceGrid(){return (Grid.xyz+1+workShape()-1)/workShape();}
uint3 workDensityGrid(){return (Grid.xyz+workShape()-1)/workShape();}
uint workProduct(uint3 g){return g.x*g.y*g.z;}
uint workIndex(uint3 p,uint3 g){return (p.z*g.y+p.y)*g.x+p.x;}
uint3 workCoordinate(uint id,uint3 g){return uint3(id%g.x,(id/g.x)%g.y,id/(g.x*g.y));}
uint workFaceCapacity(){return 3*workProduct(workFaceGrid());}
uint workFaceFlags(){return 16;}
uint workFaceList(){return 16+workFaceCapacity();}
uint workDensityList(){return 16+2*workFaceCapacity();}
uint workDensityFlags(){return workDensityList()+workProduct(workDensityGrid());}
uint workSize(){return workDensityFlags()+workProduct(workDensityGrid());}
uint workGroup(uint3 group){return group.x+group.y*65535;}
#endif
