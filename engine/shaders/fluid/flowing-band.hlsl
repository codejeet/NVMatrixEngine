#define FLUID_PARTICLE_AUTHORITY 1
#include "common.hlsli"
cbuffer FlowingBandFrame : register(b1) {
    uint4 Coarse,ImportanceGrid,Control,Policy;
    float4 Tolerance; // speed, centroid/h, covariance/h^2, density fraction
}
RWStructuredBuffer<double4> GridQuantity : register(u38);
RWStructuredBuffer<float2> FineVolume : register(u39);
RWStructuredBuffer<double2> CoarseVolume : register(u40);
struct Complexity {float4 importance,dynamics,velocity;uint4 state;};
RWStructuredBuffer<Complexity> Importance : register(u41);
RWStructuredBuffer<double4> FineQuantity : register(u42);
struct BandState {float4 error,flow;uint4 decision;};
RWStructuredBuffer<BandState> Candidate : register(u43);
RWStructuredBuffer<BandState> History : register(u44);
RWStructuredBuffer<uint> Requests : register(u45);
RWStructuredBuffer<float4> Sites : register(u46);
RWStructuredBuffer<uint> SiteCounts : register(u47);
static const uint Invalid=1,SurfaceBand=2,SolidBand=4,VelocityDetail=8,SpatialDetail=16;
static const uint PhysicsImportance=32,Padding=64,PromotionDelay=128,ForcedParticles=256;
uint coarseIndex(uint3 p){return (p.z*Coarse.y+p.y)*Coarse.x+p.x;}
uint3 coarseCoord(uint id){return uint3(id%Coarse.x,(id/Coarse.x)%Coarse.y,id/(Coarse.x*Coarse.y));}
bool finite64(double4 q){uint4 lo,hi;asuint(q,lo,hi);return all((hi&0x7ff00000u)!=0x7ff00000u);}
bool validQ(double4 q){return finite64(q)&&q.w>=0&&(q.w>0||all(q.xyz==0));}
double square64(double3 v){return v.x*v.x+v.y*v.y+v.z*v.z;}
double4 invalidQ(){return double4(asdouble(0,0x7ff80000u),0,0,0);}
[numthreads(64,1,1)]void FlowingBandMass(uint id:SV_DispatchThreadID){
    if(id>=Grid.w)return;
    uint coarse=coarseIndex(cellFromIndex(id)/2);double4 q=GridQuantity[coarse];
    double volume=CoarseVolume[coarse].x;float v=FineVolume[id].x;
    if(!validQ(q)||!finite64(volume.xxxx)||volume<0||!isfinite(v)||v<0||(volume==0&&q.w>0)){
        FineQuantity[id]=invalidQ();return;
    }
    q=volume>0?q*(double(v)/volume):0;
    uint start=CellOffsets[id],end=CellOffsets[id+1];
    if(end<start||end>Policy.w){FineQuantity[id]=invalidQ();return;}
    for(uint j=start;j<end;j++){
        uint pid=SortedIndices[j];if(pid>=Policy.w){q=invalidQ();break;}
        double4 p=ParticleQuantity[pid];
        if(!Particles[pid].velocityFlags.w||!validQ(p)||p.w<=0){q=invalidQ();break;}
        q+=p;
    }
    FineQuantity[id]=q;
}
[numthreads(64,1,1)]void FlowingBandMeasure(uint id:SV_DispatchThreadID){
    if(id>=Coarse.w)return;
    BandState result=(BandState)0;uint reason=0;uint3 base=coarseCoord(id)*2;
    float h=DomainMinCell.w;double h2=double(h)*h,full=8*h2*h;
    double4 q=GridQuantity[id];double capacity=CoarseVolume[id].x;
    if(!validQ(q)||!finite64(capacity.xxxx)||capacity<=0||any(base+1>=Grid.xyz))reason|=Invalid;
    if(reason){result.decision.y=reason;Candidate[id]=result;return;}
    if(abs(capacity-full)>full*1e-6)reason|=SolidBand;
    float3 center=DomainMinCell.xyz+(float3(base)+1)*h;
    uint population=0;
    [unroll]for(uint child=0;child<8;child++){
        uint c=cellIndex(base+uint3(child&1,(child>>1)&1,child>>2));
        uint begin=CellOffsets[c],end=CellOffsets[c+1];
        if(end<begin||end>Policy.w){reason|=Invalid;continue;}
        for(uint j=begin;j<end;j++){
            uint pid=SortedIndices[j];if(pid>=Policy.w){reason|=Invalid;continue;}
            double4 p=ParticleQuantity[pid];
            if(!Particles[pid].velocityFlags.w||!validQ(p)||p.w<=0){reason|=Invalid;continue;}
            q+=p;population++;
        }
    }
    if(!validQ(q)||q.w<=0||q.w>capacity)reason|=Invalid;
    if(reason&Invalid){result.decision.y=reason;Candidate[id]=result;return;}
    double3 mean=q.xyz/q.w;
    // Compare fluctuations in a comoving frame, avoiding catastrophic E[v^2]
    // - E[v]^2 cancellation. A velocity boost alone does not lose APIC detail.
    double3 centerMoment=0,diagonal=GridQuantity[id].w*(.3125*h2),offDiagonal=0,angular=0;
    double variance=0;
    if(GridQuantity[id].w>0){double3 dv=GridQuantity[id].xyz/GridQuantity[id].w-mean;
        variance+=GridQuantity[id].w*square64(dv);}
    [unroll]for(uint child=0;child<8;child++){
        uint c=cellIndex(base+uint3(child&1,(child>>1)&1,child>>2));
        for(uint j=CellOffsets[c];j<CellOffsets[c+1];j++){
            uint pid=SortedIndices[j];FluidParticle p=Particles[pid];double4 owned=ParticleQuantity[pid];
            double3 d=double3(p.positionRadius.xyz)-double3(center),dv=owned.xyz/owned.w-mean;
            double3 c0=p.apic0.xyz,c1=p.apic1.xyz,c2=p.apic2.xyz;
            if(any(!isfinite(p.positionRadius))||any(!isfinite(p.apic0))||any(!isfinite(p.apic1))||any(!isfinite(p.apic2)))reason|=Invalid;
            centerMoment+=owned.w*d;diagonal+=owned.w*d*d;
            offDiagonal+=owned.w*d.xyz*d.yzx;
            variance+=owned.w*(square64(dv)+.25*h2*(square64(c0)+square64(c1)+square64(c2)));
            angular+=owned.w*(d.yzx*dv.zxy-d.zxy*dv.yzx+.25*h2*double3(c2.y-c1.z,c0.z-c2.x,c1.x-c0.y));
        }
    }
    centerMoment/=q.w;diagonal/=q.w;offDiagonal/=q.w;
    double3 covariance=diagonal-centerMoment*centerMoment-(.3125*h2);
    double3 crossCovariance=offDiagonal-centerMoment.xyz*centerMoment.yzx;
    float speed=sqrt(float(max(0,variance/q.w)));
    float positionError=length(float3(centerMoment/double(h)));
    float covarianceError=float(max(max(max(abs(covariance.x),abs(covariance.y)),abs(covariance.z)),
        max(max(abs(crossCovariance.x),abs(crossCovariance.y)),abs(crossCovariance.z)))/h2);
    float angularSpeed=length(float3(angular/(q.w*h)));
    float hysteresis=(!Control.x&&History[id].decision.x)?1.5:1;
    if(speed>Tolerance.x*hysteresis||angularSpeed>Tolerance.x*hysteresis)reason|=VelocityDetail;
    if(positionError>Tolerance.y*hysteresis||covarianceError>Tolerance.z*hysteresis)reason|=SpatialDetail;
    if(Control.z){uint3 p=(base+2)/4;
        if(any(p>=ImportanceGrid.xyz))reason|=Invalid;
        else if(Importance[(p.z*ImportanceGrid.y+p.y)*ImportanceGrid.x+p.x].state.x==0)reason|=PhysicsImportance;
    }
    // Current occupancy/geometry always veto stale or frozen requested LOD.
    // Expand for travel during this step; excessive travel fails closed.
    uint travel=uint(min(3,ceil(length(float3(mean))*max(0,GravityDt.w)/h)));
    if(travel>2)reason|=SurfaceBand;
    int margin=int(Policy.x+min(travel,2));
    for(int z=-margin;z<2+margin;z++)for(int y=-margin;y<2+margin;y++)for(int x=-margin;x<2+margin;x++){
        int3 p=int3(base)+int3(x,y,z);
        if(!inGrid(p)){reason|=SurfaceBand;continue;}
        uint c=cellIndex(p);double4 owned=FineQuantity[c];float v=FineVolume[c].x;
        float4 solid=SolidGrid[c];
        if(!validQ(owned)||any(!isfinite(solid))){reason|=Invalid;continue;}
        if(v<=0||abs(double(v)-h2*h)>h2*h*1e-6||solid.x<h*.5)reason|=SolidBand;
        // A moving collider can enter during the next update. This is a swept
        // margin on current SDF samples, not a test of water's absolute speed.
        if(solid.x<h*.5+length(solid.yzw)*max(0,GravityDt.w))reason|=SolidBand;
        if(v<=0||abs(owned.w-double(v))>double(v)*Tolerance.w)reason|=SurfaceBand;
        if(owned.w>0&&length(float3(owned.xyz/owned.w-mean))>Tolerance.x*hysteresis)reason|=VelocityDetail;
    }
    result.error=float4(speed,positionError,covarianceError,angularSpeed);
    result.flow=float4(float3(mean),float(q.w));result.decision=uint4(0,reason,0,population);
    Candidate[id]=result;
}
[numthreads(64,1,1)]void FlowingBandDecide(uint id:SV_DispatchThreadID){
    if(id>=Coarse.w)return;
    BandState s=Candidate[id];int3 c=int3(coarseCoord(id));int pad=int(Policy.y);
    for(int z=-pad;z<=pad;z++)for(int y=-pad;y<=pad;y++)for(int x=-pad;x<=pad;x++){
        int3 n=c+int3(x,y,z);
        if(any(n<0)||any(n>=int3(Coarse.xyz))){s.decision.y|=Padding;continue;}
        if(Candidate[coarseIndex(n)].decision.y)s.decision.y|=Padding;
    }
    uint age=Control.x?0:History[id].decision.z;
    age=s.decision.y?0:min(age+1,Policy.z);
    if(!s.decision.y&&age<Policy.z)s.decision.y|=PromotionDelay;
    if(Control.y)s.decision.y|=ForcedParticles;
    s.decision.x=s.decision.y?0:1;s.decision.z=age;
    Requests[id]=s.decision.x;History[id]=s;
}
[numthreads(64,1,1)]void FlowingBandSites(uint id:SV_DispatchThreadID){
    if(id>=Coarse.w)return;
    uint count=0;float h=DomainMinCell.w,r=DomainMaxRadius.w;
    float3 origin=DomainMinCell.xyz+float3(coarseCoord(id)*2)*h;
    // Fixed quadrature is an explicit approximation with an admission error
    // gate, not a promise to retain arbitrary particle positions/APIC moments.
    for(uint i=0;i<64;i++){
        float3 p=origin+(float3(i&3,(i>>2)&3,i>>4)+.5)*(.5*h);
        bool valid=all(p>=DomainMinCell.xyz+r)&&all(p<=DomainMaxRadius.xyz-r);
        for(uint c=0;c<Collision.x&&valid;c++){
            float phi=colliderPhi(Colliders[c],p,r);valid=isfinite(phi)&&phi>=r;
        }
        if(valid)Sites[id*64+count++]=float4(p,0);
    }
    // ExchangeRestore owns the all-or-nothing free-slot reservation. A fully
    // blocked cell keeps its grid quantity; site generation never deletes it.
    SiteCounts[id]=count;
}
[numthreads(64,1,1)]void FlowingBandRestoreRequests(uint id:SV_DispatchThreadID){
    if(id>=Coarse.w)return;
    BandState s=(BandState)0;if(!Control.x)s=History[id];
    s.decision.x=0;s.decision.y|=ForcedParticles;s.decision.z=0;
    History[id]=s;Requests[id]=0;
}
