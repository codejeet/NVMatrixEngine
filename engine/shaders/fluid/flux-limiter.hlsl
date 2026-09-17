// Original conservative receiver-capacity limiter. Not a VOF reconstruction or
// pressure solve. A rejected transfer stays at its donor, including momentum.
cbuffer LimiterFrame : register(b0) { uint4 Grid; };
RWStructuredBuffer<float4> Resident : register(u0);
RWStructuredBuffer<float2> Capacity : register(u1);
RWStructuredBuffer<float4> Transfers : register(u2);
RWStructuredBuffer<float> Scales : register(u3);
RWByteAddressBuffer Control : register(u4);
RWByteAddressBuffer Arguments : register(u5);
uint index(uint3 p){return (p.z*Grid.y+p.y)*Grid.x+p.x;}
uint3 coord(uint id){return uint3(id%Grid.x,(id/Grid.x)%Grid.y,id/(Grid.x*Grid.y));}
uint stride(){return (Grid.x+1)*(Grid.y+1)*(Grid.z+1);}
uint face(uint3 p,uint a){return a*stride()+(p.z*(Grid.y+1)+p.y)*(Grid.x+1)+p.x;}
double tolerance(uint id){return max(1e-12,double(Capacity[id].x)*2e-7);}
void flows(uint id,out double incoming,out double outgoing){
    uint3 p=coord(id);incoming=outgoing=0;
    [unroll]for(uint a=0;a<3;a++){
        uint3 r=p;r[a]++;
        double l=Transfers[face(p,a)].w,h=Transfers[face(r,a)].w;
        incoming+=max(0,l)+max(0,-h);outgoing+=max(0,-l)+max(0,h);
    }
}
[numthreads(16,1,1)]void FluxLimitClear(uint id:SV_DispatchThreadID){Control.Store(id*4,0);}
[numthreads(1,1,1)]void FluxLimitBegin(){Control.Store(0,0);Control.Store(4,0);Control.InterlockedAdd(28,1);}
[numthreads(128,1,1)]void FluxLimitEvaluate(uint id:SV_DispatchThreadID){
    if(id>=Grid.w)return;
    double incoming,outgoing;flows(id,incoming,outgoing);
    // Do not silently repair old excess from a newly closing solid. The bound
    // is current open capacity OR existing inventory, whichever is greater.
    double room=max(double(Capacity[id].x),double(Resident[id].w))-Resident[id].w+outgoing;
    float scale=1;
    if(incoming>room+tolerance(id)){
        scale=float(max(0,room)/incoming)*(1-2e-7);
        Control.InterlockedAdd(0,1);
    }
    Scales[id]=scale;
    if(!isfinite(scale)||scale<0||scale>1)Control.InterlockedAdd(12,1);
}
[numthreads(1,1,1)]void FluxLimitPrepare(){
    bool active=Control.Load(0)>0;
    Arguments.Store3(0,uint3(active?(3*stride()+127)/128:0,1,1));
    Arguments.Store3(12,uint3(active?(Grid.w+127)/128:0,1,1));
    Arguments.Store2(32,uint2(active?1:0,0)); // DX12 aligned 64-bit predicate
    Control.Store(40,active?1:0);
    Control.Store(0,0);
}
[numthreads(128,1,1)]void FluxLimitFaces(uint id:SV_DispatchThreadID){
    if(id>=3*stride())return;
    if(id==0){Control.InterlockedAdd(4,1);Control.InterlockedAdd(8,1);}
    uint a=id/stride(),k=id%stride();
    uint3 p=uint3(k%(Grid.x+1),(k/(Grid.x+1))%(Grid.y+1),k/((Grid.x+1)*(Grid.y+1)));
    uint3 extent=Grid.xyz;extent[a]++;
    if(any(p>=extent)||p[a]==0||p[a]==Grid[a])return;
    float4 q=Transfers[id];uint3 receiver=p;if(q.w<0)receiver[a]--;
    float scale=Scales[index(receiver)];
    if(scale<1&&q.w!=0){Transfers[id]=q*scale;Control.InterlockedAdd(32,1);}
}
[numthreads(128,1,1)]void FluxLimitAudit(uint id:SV_DispatchThreadID){
    if(id>=Grid.w)return;
    double incoming,outgoing;flows(id,incoming,outgoing);
    double old=Resident[id].w,cap=Capacity[id].x,next=old+incoming-outgoing;
    // Also reproduce BulkUpdate's actual FP32 accumulation order. An accurate
    // flux sum alone must not conceal a bad stored cell value.
    uint3 p=coord(id);float4 stored=Resident[id];
    [unroll]for(uint a=0;a<3;a++){uint3 r=p;r[a]++;stored+=Transfers[face(p,a)]-Transfers[face(r,a)];}
    float excess=float(max(0,max(next,double(stored.w))-max(old,cap))),oldExcess=float(max(0,old-cap));
    Control.InterlockedMax(16,asuint(excess));Control.InterlockedMax(20,asuint(oldExcess));
    if(!isfinite(float(next))||next<0||stored.w<0||any(!isfinite(stored))||excess>max(1e-11,double(Capacity[id].x)*1e-6))Control.InterlockedAdd(12,1);
}
[numthreads(1,1,1)]void FluxLimitFinish(){
    if(Control.Load(40)>0)Control.InterlockedAdd(24,1);
    Control.InterlockedMax(36,Control.Load(4));
}
