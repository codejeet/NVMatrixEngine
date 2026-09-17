// Consume the real mixed MAC leaf map, row relaxation metric and cut geometry.
// lambda enforces the physical mixed-row pressure equation; p enforces phase capacity.
#include "mac-row.hlsli"
#include "cut-pressure-row.hlsli"
RWStructuredBuffer<uint> MacMap:register(u14);
RWStructuredBuffer<uint> MacState:register(u15);
RWStructuredBuffer<MacRow> MacRows:register(u16);
RWStructuredBuffer<CutPressureRow> MacExact:register(u17);
RWStructuredBuffer<double> LambdaIn:register(u18);
RWStructuredBuffer<double> LambdaOut:register(u19);
RWStructuredBuffer<double> PressureRates:register(u20);
uint macWidth(int3 p){return inside(p,Fine.xyz)&&(MacState[index(uint3(p)/2,Coarse.xyz)]&1)?2:1;}
double macSupport(uint id){float2 v=FineVolume[id];return (uint(Parameters.w)&1)?.5*(double(v.x)+v.y):double(v.x);}
bool macOpen(uint3 p,uint a){
    int3 l=int3(p);l[a]--;
    return inside(l,Fine.xyz)&&inside(p,Fine.xyz)&&macSupport(index(l,Fine.xyz))>0&&
        macSupport(index(p,Fine.xyz))>0&&Aperture[face(p,a,Fine.xyz)]>0;
}
double macFineWidth(uint3 p,uint a){return max(0,min(double(MinimumCell.w),double(Maximum[a])-(double(MinimumCell[a])+p[a]*double(MinimumCell.w))));}
double macFaceWeight(uint3 p,uint a){
    if(!macOpen(p,a))return 0;uint3 l=p;l[a]--;
    uint wl=macWidth(l),wr=macWidth(p);
    double distance=max(wl,wr)>1?.5*(wl+wr)*double(MinimumCell.w):.5*(macFineWidth(l,a)+macFineWidth(p,a));
    return double(Parameters.x)*double(Aperture[face(p,a,Fine.xyz)])/distance;
}
double macPotential(uint3 p,bool includeCoarse){
    uint id=index(p,Fine.xyz);
    // Store TOTAL liquid pressure in lambda. Capacity pressure controls only
    // air DOFs, avoiding cancellation of p by lambda in nearly full parents.
    return Cells[id].z==1?LambdaIn[MacMap[id]]:(includeCoarse?Potential[index(p/2,Coarse.xyz)]:0);
}
double2 macCapacityWeight(uint3 p,uint a){
    double w=macFaceWeight(p,a);if(w==0)return 0;
    int3 l=int3(p);l[a]--;uint width=max(macWidth(l),macWidth(p));
    uint3 base=p;uint b=(a+1)%3,c=(a+2)%3;
    if(width==2){base[b]&=~1u;base[c]&=~1u;}
    double2 air=0;
    [loop]for(uint y=0;y<width;y++)[loop]for(uint x=0;x<width;x++){
        uint3 r=base;r[b]+=x;r[c]+=y;uint3 lo=r;lo[a]--;
        air+=double2(Cells[index(lo,Fine.xyz)].z!=1,Cells[index(r,Fine.xyz)].z!=1);
    }
    return w*air/double(width*width);
}
double macDeltaVolume(uint3 p,uint a,bool includeCoarse){
    double w=macFaceWeight(p,a);if(w==0)return 0;
    int3 l=int3(p);l[a]--;uint width=max(macWidth(l),macWidth(p));
    uint3 base=p;uint b=(a+1)%3,c=(a+2)%3;
    if(width==2){base[b]&=~1u;base[c]&=~1u;}
    double gradient=0;
    [loop]for(uint y=0;y<width;y++)[loop]for(uint x=0;x<width;x++){
        uint3 r=base;r[b]+=x;r[c]+=y;uint3 lo=r;lo[a]--;
        gradient+=macPotential(lo,includeCoarse)-macPotential(r,includeCoarse);
    }
    return w*gradient/double(width*width);
}
double macLeafResidual(uint id){
    uint3 p=coord(id,Fine.xyz);uint size=macWidth(p);double flux=0;
    [unroll]for(uint a=0;a<3;a++)[unroll]for(uint s=0;s<2;s++){
        uint3 base=p;base[a]+=s*size;uint b=(a+1)%3,c=(a+2)%3;
        [loop]for(uint y=0;y<size;y++)[loop]for(uint x=0;x<size;x++){
            uint3 at=base;at[b]+=x;at[c]+=y;
            flux+=(s?1:-1)*(macDeltaVolume(at,a,true)+double(Parameters.x)*Canonical[face(at,a,Fine.xyz)]);
        }
    }
    if(uint(Parameters.w)&2)
        [loop]for(uint z=0;z<size;z++)[loop]for(uint y=0;y<size;y++)[loop]for(uint x=0;x<size;x++){
            float2 v=FineVolume[index(p+uint3(x,y,z),Fine.xyz)];flux+=double(v.x)-double(v.y);
        }
    return flux;
}
[numthreads(128,1,1)]void CarrierMacInitialize(uint id:SV_DispatchThreadID){
    if(id<Fine.w)LambdaIn[id]=LambdaOut[id]=0;
}
[numthreads(128,1,1)]void CarrierLiquidSweep(uint id:SV_DispatchThreadID){
    if(id>=Fine.w)return;
    if(Cells[id].z!=1||MacMap[id]!=id){LambdaOut[id]=0;return;}
    double scale=double(Parameters.x)*double(MinimumCell.w);
    // Match the actual pressure row's stable relaxation, with precise regular
    // cut-face diagonals. T junctions retain the absolute-row step from MAC.
    double step=MacExact[id].diagonal>=0?(MacExact[id].diagonal>0?.8/MacExact[id].diagonal:0):double(MacRows[id].step);
    double value=LambdaIn[id]-step*macLeafResidual(id)/scale;
    LambdaOut[id]=value;if(!isfinite(float(value)))InterlockedAdd(Control[5],1);
}
[numthreads(128,1,1)]void CarrierLiquidRhs(uint id:SV_DispatchThreadID){
    if(id>=Fine.w)return;
    LambdaOut[id]=Cells[id].z==1&&MacMap[id]==id?
        -macLeafResidual(id)/(double(Parameters.x)*double(MinimumCell.w)):0;
}
[numthreads(128,1,1)]void CarrierApplyMultigrid(uint id:SV_DispatchThreadID){
    if(id>=Fine.w)return;
    double value=LambdaIn[id]+LambdaOut[id];LambdaIn[id]=value;
    if(!isfinite(float(value)))InterlockedAdd(Control[5],1);
}
[numthreads(128,1,1)]void CarrierPressureFaces(uint id:SV_DispatchThreadID){
    if(id>=3*stride(Coarse.xyz))return;
    uint a=id/stride(Coarse.xyz);uint3 p=coord(id%stride(Coarse.xyz),Coarse.xyz+1),extent=Coarse.xyz;extent[a]++;
    if(any(p>=extent)||p[a]==0||p[a]==Coarse[a]){PressureRates[id]=0;return;}
    uint b=(a+1)%3,c=(a+2)%3;double q=0;
    [unroll]for(uint y=0;y<2;y++)[unroll]for(uint x=0;x<2;x++){
        uint3 f=p*2;f[b]+=x;f[c]+=y;if(f[b]>=Fine[b]||f[c]>=Fine[c])continue;
        q+=macDeltaVolume(f,a,false);
    }
    PressureRates[id]=Rates[id]+q/double(Parameters.x);
}
[numthreads(128,1,1)]void CarrierLiquidResidual(uint id:SV_DispatchThreadID){
    if(id>=Fine.w||Cells[id].z!=1||MacMap[id]!=id)return;
    double h=double(MinimumCell.w),volume=double(MacRows[id].volumeUnits)*h*h*h;
    // Solve the actual pressure equation, including its original residual and
    // moving-solid source. Freezing that residual can make saturated capacity
    // constraints inconsistent even when the original pressure solve passed.
    float error=float(abs(macLeafResidual(id))/(double(Parameters.x)*max(1e-20,volume))*double(Parameters.y)/1e-6);
    if(!isfinite(error))InterlockedAdd(Control[5],1);
    InterlockedMax(Control[0],asuint(error));
    InterlockedMax(Control[17],asuint(error*float(1e-6/double(Parameters.y))));
}
