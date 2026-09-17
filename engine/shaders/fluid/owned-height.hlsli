// Original local height reconstruction from volume integrals. A valid column
// crosses exactly one monotone interface with full/empty endpoints. No height
// is invented for an unresolved sheet, opposing interfaces or an incomplete band.
// Codes are +/- (height in fine-cell coordinates + 1); zero means unavailable.
RWStructuredBuffer<double4> OwnedSurfaceHeights : register(u21);
double ownedColumn(uint3 owner,uint axis) {
    uint3 grid=(SimulationGrid.xyz+1)/2;
    uint first=owner[axis]>3?owner[axis]-3:0,last=min(owner[axis]+3,grid[axis]-1);
    uint3 p=owner;p[axis]=first;double2 a=OwnedSurfacePhase[ownedSurfaceIndex(p)];
    p[axis]=last;double2 b=OwnedSurfacePhase[ownedSurfaceIndex(p)];
    if(a.y<=0.0||b.y<=0.0)return 0.0;
    bool positive=a.x==a.y&&b.x==0.0,negative=a.x==0.0&&b.x==b.y;
    if(!positive&&!negative)return 0.0;
    double height=double(first*2),previous=1.0;
    for(uint i=first;i<=last;++i){
        p[axis]=i;double2 phase=OwnedSurfacePhase[ownedSurfaceIndex(p)];
        if(phase.y<=0.0)return 0.0;
        double f=positive?phase.x/phase.y:1.0-phase.x/phase.y;
        if(f>previous+2e-12)return 0.0;
        if(f>0.0&&f<1.0){double direction=OwnedSurfacePlane[ownedSurfaceIndex(p)][axis];
            if(positive?direction<=0.0:direction>=0.0)return 0.0;}
        height+=f*double(min(2u,SimulationGrid[axis]-2*i));previous=f;
    }
    return (height+1.0)*(positive?1.0:-1.0);
}
[numthreads(128,1,1)]
void SurfaceHeights(uint id:SV_DispatchThreadID) {
    uint3 grid=(SimulationGrid.xyz+1)/2;
    if(id>=grid.x*grid.y*grid.z)return;
    uint3 p=uint3(id%grid.x,id/grid.x%grid.y,id/(grid.x*grid.y));
    double4 result=double4(ownedColumn(p,0),ownedColumn(p,1),ownedColumn(p,2),0);
    OwnedSurfaceHeights[id]=result;
    uint valid=(result.x!=0.0?1u:0u)+(result.y!=0.0?1u:0u)+(result.z!=0.0?1u:0u);
    if(valid)Counters.InterlockedAdd(16,valid);
}
// Moment weights evaluate a quadratic from three CELL AVERAGES, rather than
// treating those averages as center samples. Includes odd-width edge owners.
void ownedHeightWeights(uint start,uint axis,double position,out double3 value,out double3 derivative) {
    double3 mean,second;
    [unroll]for(uint i=0;i<3;++i){double width=double(min(2u,SimulationGrid[axis]-2*(start+i)));
        mean[i]=double(2*(start+i))+width*.5-position;second[i]=mean[i]*mean[i]+width*width/12.0;}
    double a=mean.x-mean.z,b=mean.y-mean.z,c=second.x-second.z,d=second.y-second.z;
    double determinant=a*d-b*c;
    value.x=(-mean.z*d+b*second.z)/determinant;
    value.y=(-a*second.z+mean.z*c)/determinant;value.z=1.0-value.x-value.y;
    derivative.x=d/determinant;derivative.y=-c/determinant;derivative.z=-derivative.x-derivative.y;
}
bool ownedHeightPosition(float3 p,double3 normal,out float position) {
    position=0;uint3 grid=(SimulationGrid.xyz+1)/2;
    double3 absolute=double3(normal.x<0.0?-normal.x:normal.x,normal.y<0.0?-normal.y:normal.y,normal.z<0.0?-normal.z:normal.z);
    uint axis=absolute.y>absolute.x?1:0;axis=absolute.z>absolute[axis]?2:axis;
    uint u=(axis+1)%3,v=(axis+2)%3;
    if(grid[u]<3||grid[v]<3)return false;
    uint3 owner=uint3(clamp(int3(floor(p*.5)),0,int3(grid)-1));
    uint startU=uint(clamp(int(owner[u])-1,0,int(grid[u])-3));
    uint startV=uint(clamp(int(owner[v])-1,0,int(grid[v])-3));
    double3 wu,wv,du,dv;ownedHeightWeights(startU,u,double(p[u]),wu,du);ownedHeightWeights(startV,v,double(p[v]),wv,dv);
    double height=0.0,slopeU=0.0,slopeV=0.0;bool positive=normal[axis]>0.0;
    [unroll]for(uint j=0;j<3;++j)[unroll]for(uint i=0;i<3;++i){
        owner[u]=startU+i;owner[v]=startV+j;
        double code=OwnedSurfaceHeights[ownedSurfaceIndex(owner)][axis];
        if(code==0.0||(code>0.0)!=positive)return false;
        double h=(code<0.0?-code:code)-1.0;
        height+=wu[i]*wv[j]*h;slopeU+=du[i]*wv[j]*h;slopeV+=wu[i]*dv[j]*h;
    }
    position=float((double(p[axis])-height)*(positive?1.0:-1.0))/sqrt(float(1.0+slopeU*slopeU+slopeV*slopeV));
    return isfinite(position);
}
