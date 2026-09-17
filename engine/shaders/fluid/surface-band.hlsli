// Magnitude is not distance in a weighted-center field. Keep canonical FINE
// zero-crossing masks by spatial brick ID, including during coarse gathers.
uint surfaceFineMaskBase(uint brick){return 3*Bricks.w+SimulationGrid.w+brick*16;}
groupshared uint SurfaceFineMask[16];
[numthreads(128,1,1)]void SurfaceLodMasks(uint slot:SV_GroupID,uint lane:SV_GroupIndex){
    uint brick=BrickList[slot],base=surfaceFineMaskBase(brick);
    if(!SurfaceLodNext[brick].state.z){
        if(lane<16)Counters.InterlockedAdd(80,countbits(SurfaceLodLists[base+lane]));
        return;
    }
    if(lane<16)SurfaceFineMask[lane]=0;
    GroupMemoryBarrierWithGroupSync();
    for(uint n=lane;n<512;n+=128){uint3 cell=uint3(n%8,(n/8)%8,n/64);float lo=1e30,hi=-1e30;
        [unroll]for(uint k=0;k<8;++k){uint3 p=cell+uint3(k&1,(k>>1)&1,k>>2);
            float phi=Field[slot*729+surfaceNodeIndex(p)].x;lo=min(lo,phi);hi=max(hi,phi);}
        if(lo<=0&&hi>=0)InterlockedOr(SurfaceFineMask[n/32],1u<<(n%32));
    }
    GroupMemoryBarrierWithGroupSync();
    if(lane<16){SurfaceLodLists[base+lane]=SurfaceFineMask[lane];Counters.InterlockedAdd(80,countbits(SurfaceFineMask[lane]));}
}
// Immutable 3x3x3 brick neighborhood. Each analysis group loads it once for
// cross-brick shading and photon-differential stencils.
groupshared uint SurfaceNeighborhoodMasks[432];
void surfaceLoadBand(uint brick,uint lane){
    int3 b=int3(brickCoord(brick));
    for(uint n=lane;n<432;n+=128){uint neighbor=n/16;
        int3 p=b+int3(neighbor%3,(neighbor/3)%3,neighbor/9)-1;uint mask=0;
        if(all(p>=0)&&all(p<int3(Bricks.xyz))){uint id=(p.z*Bricks.y+p.y)*Bricks.x+p.x;
            if(BrickMap[id]!=0xffffffff)mask=SurfaceLodLists[surfaceFineMaskBase(id)+n%16];}
        SurfaceNeighborhoodMasks[n]=mask;
    }
    GroupMemoryBarrierWithGroupSync();
}
bool surfaceInBand(uint3 node){
    // Surface-cell corners plus one node in every direction contain both the
    // filtered normal and +/- half-cell photon normal-differential stencil.
    uint3 lo=node+6,hi=node+9;
    for(uint z=lo.z;z<=hi.z;++z)for(uint y=lo.y;y<=hi.y;++y)
        for(uint bx=lo.x/8;bx<=hi.x/8;++bx){
            uint first=max(lo.x,bx*8)-bx*8,last=min(hi.x,bx*8+7)-bx*8;
            uint neighbor=(z/8*3+y/8)*3+bx,row=(z%8)*64+(y%8)*8;
            uint bits=((1u<<(last-first+1))-1u)<<(row%32+first);
            if(SurfaceNeighborhoodMasks[neighbor*16+row/32]&bits)return true;
        }
    return false;
}
bool surfaceInBandGlobal(uint brick,uint3 node){
    int3 p=int3(brickCoord(brick)*8+node),lo=max(p-2,0),hi=min(p+1,int3(Bricks.xyz*8)-1);
    for(int z=lo.z;z<=hi.z;++z)for(int y=lo.y;y<=hi.y;++y)for(int x=lo.x;x<=hi.x;++x){
        uint3 cell=uint3(x,y,z),b=cell/8;uint id=(b.z*Bricks.y+b.y)*Bricks.x+b.x;
        if(BrickMap[id]==0xffffffff)continue;
        uint n=(cell.z%8)*64+(cell.y%8)*8+cell.x%8;
        if(SurfaceLodLists[surfaceFineMaskBase(id)+n/32]&(1u<<(n%32)))return true;
    }
    return false;
}
