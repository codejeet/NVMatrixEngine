// Backward-Euler upwind transport of volume and all momentum components.
// Capacity is the actual endpoint geometry, not a padded epsilon cell volume.
#include "bulk-numeric.hlsli"
cbuffer ImplicitFrame : register(b0){uint4 Grid;float4 Parameters;};
RWStructuredBuffer<Bulk4> Resident : register(u0);
RWStructuredBuffer<Bulk2> Capacity : register(u1);
RWStructuredBuffer<double> Rates : register(u2);
RWStructuredBuffer<double4> SolutionIn : register(u3);
RWStructuredBuffer<double4> SolutionOut : register(u4);
RWStructuredBuffer<double> Diagonal : register(u5);
RWStructuredBuffer<Bulk4> Output : register(u6);
RWStructuredBuffer<Bulk4> Transfers : register(u7);
RWStructuredBuffer<float2> Limiter : register(u8);
RWStructuredBuffer<uint> Control : register(u9);
RWByteAddressBuffer Arguments : register(u10);
uint index(uint3 p){return (p.z*Grid.y+p.y)*Grid.x+p.x;}
uint3 coord(uint id){return uint3(id%Grid.x,(id/Grid.x)%Grid.y,id/(Grid.x*Grid.y));}
uint stride(){return (Grid.x+1)*(Grid.y+1)*(Grid.z+1);}
uint face(uint3 p,uint a){return a*stride()+(p.z*(Grid.y+1)+p.y)*(Grid.x+1)+p.x;}
double flow(uint3 p,uint a){return double(Parameters.x)*Rates[face(p,a)];}
double4 source(uint id){
    uint3 p=coord(id);double4 rhs=Resident[id];
    [unroll]for(uint a=0;a<3;a++){
        uint3 lo=p,hi=p;lo[a]--;hi[a]++;
        double l=flow(p,a),r=flow(hi,a);
        if(p[a]>0&&l>0)rhs+=l*SolutionIn[index(lo)];
        if(hi[a]<Grid[a]&&r<0)rhs-=r*SolutionIn[index(hi)];
    }
    return rhs;
}
[numthreads(16,1,1)]void ImplicitClear(uint id:SV_DispatchThreadID){Control[id]=0;}
[numthreads(128,1,1)]void ImplicitInitialize(uint id:SV_DispatchThreadID){
    if(id==0){Control[0]=Control[1]=Control[2]=0;Control[13]=1;}
    if(id>=Grid.w)return;
    uint3 p=coord(id);double outflow=0;
    [unroll]for(uint a=0;a<3;a++){uint3 hi=p;hi[a]++;outflow+=max(0,-flow(p,a))+max(0,flow(hi,a));}
    Diagonal[id]=double(Capacity[id].x)+outflow;
    // A zero row with a nonzero source is inconsistent, not slow to converge.
    // Preserve the input on failure instead of spending the full iteration cap.
    if(Diagonal[id]==0&&any(Resident[id]!=0))InterlockedAdd(Control[5],1);
    SolutionIn[id]=SolutionOut[id]=0;
    double start=Parameters.w!=0?Capacity[id].y:Capacity[id].x;
    Limiter[id]=float2(1,start>0?float(outflow/start):0); // diagnostic only: no donor cap
    if(start>0&&Capacity[id].x==0){InterlockedAdd(Control[7],1);if(Resident[id].w>0)InterlockedAdd(Control[8],1);}
}
[numthreads(128,1,1)]void ImplicitIterate(uint id:SV_DispatchThreadID){
    if(id==0)InterlockedAdd(Control[2],1);
    if(id>=Grid.w)return;
    double d=Diagonal[id];double4 value=d>0?source(id)/d:0;
    SolutionOut[id]=value;
    if(any(!isfinite(float4(value)))||value.w<0)InterlockedAdd(Control[5],1);
}
[numthreads(128,1,1)]void ImplicitResidual(uint id:SV_DispatchThreadID){
    if(id>=Grid.w)return;
    double4 residual=abs(Diagonal[id]*SolutionIn[id]-source(id));
    double4 scale=max(double(Parameters.z),abs(double4(Resident[id]))+Diagonal[id]);
    double4 e=residual/scale;float error=float(max(max(e.x,e.y),max(e.z,e.w)));
    if(!isfinite(error))InterlockedAdd(Control[5],1);
    InterlockedMax(Control[0],asuint(error));
}
[numthreads(1,1,1)]void ImplicitPrepare(){
    float error=asfloat(Control[0]);bool active=error>Parameters.y&&Control[5]==0;
    Control[1]=Control[0];Control[0]=0;Control[13]=active?1:0;
    Arguments.Store3(0,uint3(active?(Grid.w+127)/128:0,1,1));
    Arguments.Store2(32,uint2(active?1:0,0));
}
[numthreads(128,1,1)]void ImplicitExport(uint id:SV_DispatchThreadID){
    // A failed solve must not overwrite resident mass with an unconverged
    // answer. The CPU reports the failure at its existing frame fence.
    bool failed=Control[13]!=0||Control[5]!=0;
    if(id<Grid.w){
        Bulk4 next=failed?Resident[id]:Bulk4(double(Capacity[id].x)*SolutionIn[id]);
        Output[id]=next;
        if(any(!isfinite(float4(next)))||next.w<0)InterlockedAdd(Control[14],1);
        float excess=float(max(0,next.w-Capacity[id].x)),fraction=max(0,float(SolutionIn[id].w));
        InterlockedMax(Control[11],asuint(excess));InterlockedMax(Control[12],asuint(fraction));
    }
    if(id>=3*stride())return;
    uint a=id/stride(),k=id%stride();uint3 p=uint3(k%(Grid.x+1),(k/(Grid.x+1))%(Grid.y+1),k/((Grid.x+1)*(Grid.y+1)));
    uint3 extent=Grid.xyz;extent[a]++;
    if(failed||any(p>=extent)||p[a]==0||p[a]==Grid[a]){Transfers[id]=0;return;}
    double q=double(Parameters.x)*Rates[id];uint3 donor=p;if(q>=0)donor[a]--;
    Transfers[id]=Bulk4(q*SolutionIn[index(donor)]);
}
[numthreads(1,1,1)]void ImplicitFinish(){
    Control[5]+=Control[14];Control[14]=0;
    Control[3]+=Control[2];Control[4]++;Control[10]=max(Control[10],Control[2]);
    Control[9]=max(Control[9],Control[1]);if(Control[13])Control[6]++;
}
