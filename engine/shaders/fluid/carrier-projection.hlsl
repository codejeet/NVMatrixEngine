// Capacity-constrained carrier extension. Default mode preserves liquid flux;
// the opt-in coupled mode solves the actual mixed-MAC pressure equation too.
#ifndef CARRIER_COUPLED
#define CARRIER_COUPLED 0
#endif
cbuffer CarrierFrame:register(b0){uint4 Fine,Coarse;float4 MinimumCell,Maximum;float4 Parameters;}
cbuffer CarrierStage:register(b1){uint Colour;}
#if CARRIER_COUPLED
#define BULK_PRECISE 1
#endif
#include "bulk-numeric.hlsli"
RWStructuredBuffer<Bulk4> Resident:register(u0);
RWStructuredBuffer<Bulk2> Capacity:register(u1);
RWStructuredBuffer<float2> FineVolume:register(u2);
RWStructuredBuffer<float4> Cells:register(u3);
RWStructuredBuffer<float> Aperture:register(u4);
RWStructuredBuffer<double> Canonical:register(u5);
RWStructuredBuffer<double> Extended:register(u6);
#if CARRIER_COUPLED
RWStructuredBuffer<double2> Weights:register(u7); // independent left/right air-pressure coefficients
double2 capacityWeights(uint id){return Weights[id];}
#else
RWStructuredBuffer<double> Weights:register(u7);
double2 capacityWeights(uint id){return double2(Weights[id],Weights[id]);}
#endif
RWStructuredBuffer<double> Rates:register(u8);
RWStructuredBuffer<double4> Rows:register(u9); // all-liquid reference RHS, air conductance, residual scale, unused
RWStructuredBuffer<double> Potential:register(u10);
RWStructuredBuffer<uint> Control:register(u11);
RWByteAddressBuffer Arguments:register(u12);
RWStructuredBuffer<double> Fraction:register(u13); // trial phase; never resident inventory
uint index(uint3 p,uint3 n){return (p.z*n.y+p.y)*n.x+p.x;}
uint3 coord(uint i,uint3 n){return uint3(i%n.x,(i/n.x)%n.y,i/(n.x*n.y));}
uint stride(uint3 n){return (n.x+1)*(n.y+1)*(n.z+1);}
uint face(uint3 p,uint a,uint3 n){return a*stride(n)+index(p,n+1);}
bool inside(int3 p,uint3 n){return all(p>=0)&&all(p<int3(n));}
double width(uint3 p,uint a){return max(0,min(2*double(MinimumCell.w),double(Maximum[a])-(double(MinimumCell[a])+p[a]*2*double(MinimumCell.w))));}
#if CARRIER_COUPLED
#include "carrier-mac.hlsli"
#endif
bool eligible(uint3 p,uint a){
    int3 left=int3(p);left[a]--;
    if(!inside(left,Fine.xyz)||!inside(p,Fine.xyz)||p[a]%2!=0)return false;
    uint l=index(left,Fine.xyz),r=index(p,Fine.xyz);
    float2 vl=FineVolume[l],vr=FineVolume[r];
    float sl=(uint(Parameters.w)&1)?vl.x+vl.y:vl.x,sr=(uint(Parameters.w)&1)?vr.x+vr.y:vr.x;
    return sl>0&&sr>0&&(CARRIER_COUPLED||(Cells[l].z==0&&Cells[r].z==0))&&Aperture[face(p,a,Fine.xyz)]>0;
}
double weight(uint3 p,uint a){
    if(!eligible(p,a))return 0;
#if CARRIER_COUPLED
    return macFaceWeight(p,a);
#else
    uint3 r=p/2,l=r;l[a]--;
    return double(Parameters.x)*double(Aperture[face(p,a,Fine.xyz)])/(.5*(width(l,a)+width(r,a)));
#endif
}
[numthreads(16,1,1)]void CarrierClear(uint id:SV_DispatchThreadID){Control[id]=0;}
[numthreads(128,1,1)]void CarrierFaces(uint id:SV_DispatchThreadID){
    if(id>=3*stride(Coarse.xyz))return;
    uint a=id/stride(Coarse.xyz);uint3 p=coord(id%stride(Coarse.xyz),Coarse.xyz+1),extent=Coarse.xyz;extent[a]++;
    if(any(p>=extent)||p[a]==0||p[a]==Coarse[a]){Weights[id]=0;Rates[id]=0;return;}
    uint b=(a+1)%3,c=(a+2)%3;double2 w=0;double rate=0;
    [unroll]for(uint y=0;y<2;y++)[unroll]for(uint x=0;x<2;x++){
        uint3 f=p*2;f[b]+=x;f[c]+=y;if(f[b]>=Fine[b]||f[c]>=Fine[c])continue;
        rate+=Canonical[face(f,a,Fine.xyz)];
#if CARRIER_COUPLED
        double2 v=macCapacityWeight(f,a);
#else
        double2 v=weight(f,a).xx;
#endif
        w+=v;if(any(v>0))InterlockedAdd(Control[7],1);
    }
#if CARRIER_COUPLED
    Weights[id]=w;
#else
    Weights[id]=w.x;
#endif
    Rates[id]=rate;
}
[numthreads(128,1,1)]void CarrierRows(uint id:SV_DispatchThreadID){
    if(id==0){Control[0]=Control[1]=Control[2]=0;Control[13]=1;
#if CARRIER_COUPLED
        Control[16]=Control[17]=0;
#endif
    }
    if(id>=Coarse.w)return;uint3 p=coord(id,Coarse.xyz);double div=0,d=0,magnitude=0;
    [unroll]for(uint a=0;a<3;a++){
        uint3 hi=p;hi[a]++;uint l=face(p,a,Coarse.xyz),r=face(hi,a,Coarse.xyz);
        div+=double(Parameters.x)*(Rates[r]-Rates[l]);d+=capacityWeights(l).y+capacityWeights(r).x;
        magnitude+=double(Parameters.x)*(abs(Rates[l])+abs(Rates[r]));
    }
    double rhs=double(Resident[id].w)-double(Capacity[id].x)-div;
    double scale=max(double(Parameters.z),double(Capacity[id].x)+double(Resident[id].w)+magnitude);
    double active=0;
#if CARRIER_COUPLED
    // In a parent containing no air pressure DOF, p is redundant with the
    // unrestricted liquid multipliers. Fix that gauge at p=0. Its phase
    // balance/bounds remain independently checked; they are not waived.
    [unroll]for(uint z=0;z<2;z++)[unroll]for(uint y=0;y<2;y++)[unroll]for(uint x=0;x<2;x++){
        uint3 child=p*2+uint3(x,y,z);if(any(child>=Fine.xyz))continue;
        uint i=index(child,Fine.xyz);if(Cells[i].z==1||macSupport(i)<=0)continue;
        active=1;
        // Include INTERNAL air/liquid faces in the air-basis diagonal. They
        // vanish from coarse divergence only before the liquid block responds.
        // Omitting them underestimates the reduced operator and overshoots p.
        [unroll]for(uint a=0;a<3;a++)[unroll]for(uint s=0;s<2;s++){
            int3 neighbor=int3(child);neighbor[a]+=s?1:-1;
            if(!inside(neighbor,Fine.xyz)||any(uint3(neighbor)/2!=p))continue;
            if(Cells[index(neighbor,Fine.xyz)].z!=1)continue;
            uint3 at=child;at[a]+=s;d+=macFaceWeight(at,a);
        }
    }
#endif
    Rows[id]=double4(rhs,d,scale,active);Potential[id]=0;Fraction[id]=0;
    InterlockedMax(Control[11],asuint(float(max(0,rhs))));
}
// Oriented outward flows at zero local potential, neighbor fractions, and
// conductances. Upwinding always uses the corrected flow, including reversals.
void neighbors(uint3 p,out double q[6],out double w[6],out double c[6]){
    [unroll]for(uint a=0;a<3;a++){
        uint3 lo=p,hi=p;lo[a]--;hi[a]++;
        uint l=face(p,a,Coarse.xyz),r=face(hi,a,Coarse.xyz);
        double2 wl=capacityWeights(l),wr=capacityWeights(r);
        w[2*a]=wl.y;w[2*a+1]=wr.x;
        double pl=0,pr=0;c[2*a]=c[2*a+1]=0;
        if(p[a]>0){uint i=index(lo,Coarse.xyz);pl=Potential[i];c[2*a]=Fraction[i];}
        if(hi[a]<Coarse[a]){uint i=index(hi,Coarse.xyz);pr=Potential[i];c[2*a+1]=Fraction[i];}
#if CARRIER_COUPLED
        q[2*a]=-double(Parameters.x)*PressureRates[l]-wl.x*pl;
        q[2*a+1]=double(Parameters.x)*PressureRates[r]-wr.y*pr;
#else
        q[2*a]=-double(Parameters.x)*Rates[l]-w[2*a]*pl;
        q[2*a+1]=double(Parameters.x)*Rates[r]-w[2*a+1]*pr;
#endif
    }
}
[numthreads(128,1,1)]void CarrierSweep(uint id:SV_DispatchThreadID){
    if(id==0&&Colour==0)InterlockedAdd(Control[2],1);
    if(id>=Coarse.w)return;uint3 p=coord(id,Coarse.xyz);if(((p.x+p.y+p.z)&1)!=Colour)return;
    double q[6],w[6],c[6];neighbors(p,q,w,c);
#if CARRIER_COUPLED
    if(Rows[id].w==0){
        double d=Capacity[id].x,rhs=Resident[id].w;
        [unroll]for(uint a=0;a<6;a++){d+=max(0,q[a]);rhs-=min(0,q[a])*c[a];}
        Potential[id]=0;Fraction[id]=min(1,d>0?rhs/d:0);return;
    }
    // A global phase error must not reactivate already converged capacity
    // rows. Test this cell against the CURRENT MAC flux and neighbors, while
    // continuing donor-fraction updates at fixed potential.
    double heldD=Capacity[id].x,heldRhs=Resident[id].w;
    [unroll]for(uint a=0;a<6;a++){
        double flow=q[a]+w[a]*Potential[id];heldD+=max(0,flow);heldRhs-=min(0,flow)*c[a];
    }
    double heldResidual=heldD*Fraction[id]-heldRhs;
    if(abs(heldResidual)<=double(Parameters.y)*Rows[id].z&&heldResidual>=-1e-12*Rows[id].z){
        double d=heldD,rhs=heldRhs;
        Fraction[id]=Potential[id]>0?1:min(1,d>0?rhs/d:0);
        return;
    }
#endif
    double d=Capacity[id].x,rhs=Resident[id].w,totalW=Rows[id].y,turn=0,directW=0;
    [unroll]for(uint a=0;a<6;a++){
        d+=max(0,q[a]);rhs-=min(0,q[a])*c[a];
        directW+=w[a];
        if(w[a]>0)turn=max(turn,-q[a]/w[a]);
    }
    double proximal=CARRIER_COUPLED?max(0,totalW-directW):0,previous=Potential[id];
    double load=rhs-d+proximal*previous;
    double fraction=d>0?min(1,rhs/d):0,potential=0;
    if(load>0){
        fraction=1;
        // A small positive supersolution margin: corrected transport must not
        // repeatedly round an almost-full inventory ABOVE endpoint capacity.
        // This changes flux, not resident mass; closed cells get zero margin.
        double slack=CARRIER_COUPLED?min(.1*double(Parameters.y)*Rows[id].z,1e-8*double(Capacity[id].x)):0;
        if(totalW>0){
            // F(p)=C-qold+sum(upwind(Q0+W*p)) is convex piecewise
            // linear with at most six breakpoints. Start above every root;
            // Newton moves monotonically down and crosses at most six kinks.
            potential=max(0,turn)+(load+slack)/totalW;
            [unroll]for(uint iteration=0;iteration<8;iteration++){
                // Proximal stabilization is zero at the fixed point: no
                // artificial physical flux or inventory term is introduced.
                double f=double(Capacity[id].x)-double(Resident[id].w)-slack+proximal*(potential-previous),slope=proximal;
                [unroll]for(uint a=0;a<6;a++){
                    double flow=q[a]+w[a]*potential,up=flow>=0?1:c[a];
                    f+=flow*up;slope+=w[a]*up;
                }
                if(slope>0)potential=max(0,potential-f/slope);
            }
        }
    }
    Fraction[id]=fraction;Potential[id]=potential;
    if(!isfinite(float(potential))||!isfinite(float(fraction))||fraction<0||fraction>1)InterlockedAdd(Control[5],1);
}
[numthreads(128,1,1)]void CarrierResidual(uint id:SV_DispatchThreadID){
    if(id>=Coarse.w)return;double q[6],w[6],c[6];neighbors(coord(id,Coarse.xyz),q,w,c);
    double residual=double(Capacity[id].x)*Fraction[id]-double(Resident[id].w);
    [unroll]for(uint a=0;a<6;a++){
        double flow=q[a]+w[a]*Potential[id];residual+=flow*(flow>=0?Fraction[id]:c[a]);
    }
    float error=float(abs(residual)/Rows[id].z);
    // Count only the completed solve, not transient zero-initialized fractions.
    if(Colour==2&&Rows[id].y==0&&residual<-double(Parameters.y)*Rows[id].z)InterlockedAdd(Control[15],1);
    if(!isfinite(error))InterlockedAdd(Control[5],1);
#if CARRIER_COUPLED
    // A*c >= source certifies a bounded upper solution for the final upwind
    // M-matrix solve. An unsigned residual alone permits accumulating overfill.
    float deficit=float(max(0,-residual)/Rows[id].z*double(Parameters.y)/1e-12);
    InterlockedMax(Control[0],asuint(max(error,deficit)));
    InterlockedMax(Control[16],asuint(error));
#else
    InterlockedMax(Control[0],asuint(error));
#endif
}
[numthreads(1,1,1)]void CarrierPrepare(){
#if CARRIER_COUPLED
    Control[18+2*Control[2]]=Control[16];Control[19+2*Control[2]]=Control[17];
    Control[16]=Control[17]=0;
#endif
    Control[1]=Control[0];Control[0]=0;
    bool active=asfloat(Control[1])>Parameters.y&&Control[5]==0&&Control[15]==0;
    Control[13]=active?1:0;
    Arguments.Store3(0,uint3(active?(Coarse.w+127)/128:0,1,1));Arguments.Store2(32,uint2(active?1:0,0));
}
[numthreads(128,1,1)]void CarrierExport(uint id:SV_DispatchThreadID){
    if(id>=3*stride(Fine.xyz))return;
    double value=Canonical[id];uint a=id/stride(Fine.xyz);uint3 p=coord(id%stride(Fine.xyz),Fine.xyz+1),extent=Fine.xyz;extent[a]++;
    if(any(p>=extent)){Extended[id]=value;return;} // padding is not a physical face
    bool failed=Control[13]!=0||Control[5]!=0||Control[15]!=0;
    if(!failed&&all(p<extent)){
#if CARRIER_COUPLED
        double delta=macDeltaVolume(p,a,true)/double(Parameters.x);
#else
        double delta=0;
        if(eligible(p,a)){
        uint3 r=p/2,l=r;l[a]--;
        delta=weight(p,a)*(Potential[index(l,Coarse.xyz)]-Potential[index(r,Coarse.xyz)])/double(Parameters.x);
        }
#endif
        value+=delta;if(delta!=0)InterlockedAdd(Control[8],1);
        InterlockedMax(Control[12],asuint(float(abs(delta))));
    }
    Extended[id]=value;if(!isfinite(float(value)))InterlockedAdd(Control[14],1);
}
[numthreads(1,1,1)]void CarrierFinish(){
    Control[5]+=Control[14];Control[14]=0;Control[3]+=Control[2];Control[4]++;
    Control[9]=max(Control[9],Control[1]);Control[10]=max(Control[10],Control[2]);
    if(Control[13]||Control[15])Control[6]++;
}
