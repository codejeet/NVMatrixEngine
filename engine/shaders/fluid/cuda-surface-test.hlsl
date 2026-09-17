#include "owned-phase.hlsli"
cbuffer Test : register(b0) { uint4 FineMode; float4 TestPlane; }
RWStructuredBuffer<double2> Phase : register(u0);
RWStructuredBuffer<double4> Plane : register(u1);
RWStructuredBuffer<float4> Result : register(u2);
RWStructuredBuffer<double4> GridQuantity : register(u3);
RWStructuredBuffer<double> Capacity : register(u4);
[numthreads(64,1,1)]
void SurfaceSeed(uint3 id : SV_DispatchThreadID) {
    uint3 coarse = (FineMode.xyz+1)/2;
    if(id.x < FineMode.x*FineMode.y*FineMode.z)
        Capacity[id.x] = FineMode.w == 4 && id.x == 0 ? -1.0 : 0.125;
    if(id.x >= coarse.x*coarse.y*coarse.z) return;
    uint3 p=uint3(id.x%coarse.x, id.x/coarse.x%coarse.y, id.x/(coarse.x*coarse.y));
    uint3 widths=min(uint3(2,2,2),FineMode.xyz-2*p);
    double volume=double(widths.x*widths.y*widths.z)*0.125;
    double fraction=p.y==1 ? 0.25 : (FineMode.w==1 ? (p.y>=2 ? 1.0 : 0.0) : (p.y==0 ? 1.0 : 0.0));
    if(FineMode.w==2) fraction=0.0;
    if(FineMode.w!=4) GridQuantity[id.x]=double4(0,0,0,volume*fraction);
}
[numthreads(64,1,1)]
void SurfaceProbe(uint3 id : SV_DispatchThreadID) {
    uint3 coarse=(FineMode.xyz+1)/2;
    if(id.x >= coarse.x*coarse.y*coarse.z) return;
    double2 phase=Phase[id.x]; double4 plane=Plane[id.x];
    float mask=ownedPhaseOccupancy(phase,plane,float3(.5,.1,.5))+
        2*ownedPhaseOccupancy(phase,plane,float3(.5,.5,.5))+
        4*ownedPhaseOccupancy(phase,plane,float3(.5,.9,.5));
    Result[id.x]=float4(float(phase.x),float(phase.y),float(phase.x/phase.y),mask);
}
[numthreads(64,1,1)]
void SurfaceBoxProbe(uint3 id : SV_DispatchThreadID) {
    if(id.x>=FineMode.x)return;
    double fraction=ownedPhaseBoxFraction(Phase[id.x],Plane[id.x],
                                         GridQuantity[2*id.x].xyz,GridQuantity[2*id.x+1].xyz);
    Result[id.x]=float4(float(fraction),0,0,0);
}
[numthreads(64,1,1)]
void SurfaceGeometrySeed(uint3 id : SV_DispatchThreadID) {
    uint3 coarse=(FineMode.xyz+1)/2;
    if(id.x>=coarse.x*coarse.y*coarse.z)return;
    uint3 p=uint3(id.x%coarse.x,id.x/coarse.x%coarse.y,id.x/(coarse.x*coarse.y));
    double3 width=double3(min(uint3(2,2,2),FineMode.xyz-2*p))*.5;
    double3 normal=double3(TestPlane.xyz),n=normal*width;
    double alpha=double(TestPlane.w)-normal.x*double(p.x)-normal.y*double(p.y)-normal.z*double(p.z);
    double3 absolute=double3(n.x<0.0?-n.x:n.x,n.y<0.0?-n.y:n.y,n.z<0.0?-n.z:n.z);
    alpha-=(n.x<0.0?n.x:0.0)+(n.y<0.0?n.y:0.0)+(n.z<0.0?n.z:0.0);
    double sum=absolute.x+absolute.y+absolute.z;
    double capacity=width.x*width.y*width.z;
    Phase[id.x]=double2(capacity*ownedPlaneFraction(absolute,alpha),capacity);
    Plane[id.x]=double4(n/sum,alpha/sum);
}
// Manufactured curved surface: y = height + curvature*((x-cx)^2+(z-cz)^2).
// This bounded setup integration is a fixture, not a runtime simulation pass.
[numthreads(64,1,1)]
void SurfaceCurveSeed(uint3 id:SV_DispatchThreadID) {
    uint3 coarse=(FineMode.xyz+1)/2;
    if(id.x>=coarse.x*coarse.y*coarse.z)return;
    uint3 owner=uint3(id.x%coarse.x,id.x/coarse.x%coarse.y,id.x/(coarse.x*coarse.y));
    double h=double(asfloat(FineMode.w));
    double3 lo=double3(owner*2)*h,width=double3(min(uint3(2,2,2),FineMode.xyz-owner*2))*h;
    double3 center=lo+width*.5;
    double q=double(TestPlane.w),cx=double(TestPlane.x),cz=double(TestPlane.z),base=double(TestPlane.y);
    double minimum=1e30,maximum=-1e30;
    // The nearest point to the paraboloid axis can be inside the footprint.
    double dx0=lo.x-cx,dx1=lo.x+width.x-cx,dz0=lo.z-cz,dz1=lo.z+width.z-cz;
    double minX=dx0<=0.0&&dx1>=0.0?0.0:(dx0*dx0<dx1*dx1?dx0*dx0:dx1*dx1);
    double minZ=dz0<=0.0&&dz1>=0.0?0.0:(dz0*dz0<dz1*dz1?dz0*dz0:dz1*dz1);
    double maxX=dx0*dx0>dx1*dx1?dx0*dx0:dx1*dx1,maxZ=dz0*dz0>dz1*dz1?dz0*dz0:dz1*dz1;
    double first=base+q*(minX+minZ),last=base+q*(maxX+maxZ);
    minimum=first<last?first:last;maximum=first>last?first:last;
    double fraction;
    if(minimum>=lo.y+width.y)fraction=1.0;
    else if(maximum<=lo.y)fraction=0.0;
    else if(minimum>=lo.y&&maximum<=lo.y+width.y){
        double x=center.x-cx,z=center.z-cz;
        fraction=(base+q*(x*x+z*z+(width.x*width.x+width.z*width.z)/12.0)-lo.y)/width.y;
    }else{
        double sum=0.0;
        [loop]for(uint z=0;z<64;++z)[loop]for(uint x=0;x<64;++x){
            double dx=lo.x+width.x*(double(x)+.5)/64.0-cx,dz=lo.z+width.z*(double(z)+.5)/64.0-cz;
            double value=(base+q*(dx*dx+dz*dz)-lo.y)/width.y;
            sum+=value<0.0?0.0:(value>1.0?1.0:value);
        }
        fraction=sum/4096.0;
    }
    double3 n=double3(-2.0*q*(center.x-cx),1.0,-2.0*q*(center.z-cz))*width;
    double3 positive=double3(n.x<0.0?-n.x:n.x,n.y,n.z<0.0?-n.z:n.z);
    double span=positive.x+positive.y+positive.z,low=0.0,high=span;
    [loop]for(uint k=0;k<44;++k){double mid=(low+high)*.5;
        if(ownedPlaneFraction(positive,mid)<fraction)low=mid;else high=mid;}
    double capacity=width.x*width.y*width.z;
    Phase[id.x]=double2(fraction*capacity,capacity);
    Plane[id.x]=double4(n/span,(low+high)*.5/span);
}
