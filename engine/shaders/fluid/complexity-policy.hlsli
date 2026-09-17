#ifndef FLUID_COMPLEXITY_POLICY
#define FLUID_COMPLEXITY_POLICY
// Shared with the host numerical test. No stateful renderer/physics dependencies.
uint complexityLod(uint oldLod,float value,float p0,float p1,float p2,float d0,float d1,float d2) {
    float promote[3]={p0,p1,p2},demote[3]={d0,d1,d2};
    uint lod=oldLod>3?3:oldLod;
    for(uint i=0;i<3;++i)if(lod>0&&value>promote[lod-1])--lod;
    for(uint j=0;j<3;++j)if(lod<3&&value<demote[lod])++lod;
    return lod;
}
#endif
