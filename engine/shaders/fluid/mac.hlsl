#include "common.hlsli"
#ifndef PRECISE_MAC
#define PRECISE_MAC 0
#endif
#if PRECISE_MAC
RWStructuredBuffer<double> PrecisePressure : register(u28);
#if CUT_PRESSURE
#include "cut-pressure-row.hlsli"
RWStructuredBuffer<CutPressureRow> CutExact : register(u33);
#endif
#endif
#if CUT_PRESSURE
RWStructuredBuffer<double> CutFlux : register(u34); // canonical shared-face m^3/s; Faces is APIC's FP32 cache
RWStructuredBuffer<double> CapacityFlux : register(u35);
#endif
// A coarse leaf has one pressure and one normal velocity per large boundary
// face. The fine Faces buffer is a transfer cache, not eight pressure DOFs.
cbuffer MacFrame : register(b1) {
    uint4 CoarseGrid, ImportanceGrid, MacControl; // reset, importance, audit, forced fine
};
RWStructuredBuffer<uint> LeafMap : register(u16);
RWStructuredBuffer<uint> CoarseState : register(u17); // bit 0 actual LOD, age in bits 8+
RWStructuredBuffer<uint> ActiveLeaves : register(u18);
#include "mac-row.hlsli"
RWStructuredBuffer<MacRow> Rows : register(u19);
RWStructuredBuffer<uint> MacCounters : register(u20); // leaves, coarse, T faces, invalid
RWStructuredBuffer<uint> MacArguments : register(u21);
struct MacImportance {float4 importance,dynamics,velocity;uint4 state;};
RWStructuredBuffer<MacImportance> Importance : register(u27);
uint coarseIndex(uint3 p){return (p.z*CoarseGrid.y+p.y)*CoarseGrid.x+p.x;}
uint3 coarseCoord(uint i){return uint3(i%CoarseGrid.x,(i/CoarseGrid.x)%CoarseGrid.y,i/(CoarseGrid.x*CoarseGrid.y));}
uint leafWidth(int3 p){return inGrid(p)&&(CoarseState[coarseIndex(uint3(p)/2)]&1)?2:1;}
uint leaf(int3 p){return LeafMap[cellIndex(p)];}
uint3 faceCoord(uint id){uint k=id%faceStride();return uint3(k%(Grid.x+1),(k/(Grid.x+1))%(Grid.y+1),k/((Grid.x+1)*(Grid.y+1)));}
bool validFace(uint3 p,uint a){uint3 extent=Grid.xyz;extent[a]++;return all(p<extent);}
uint3 patchBase(uint3 p,uint a,out uint width,out float distance){
    int3 l=int3(p);l[a]--;uint wl=leafWidth(l),wr=leafWidth(p);
    width=max(wl,wr);distance=.5*(wl+wr);
    if(width==2){p[(a+1)%3]&=~1u;p[(a+2)%3]&=~1u;}
#if CUT_PRESSURE
    else if(inGrid(l)&&inGrid(p))distance=cutPressureDistance(p,a);
#endif
    return p;
}
[numthreads(64,1,1)]
void MacClear(uint3 tid:SV_DispatchThreadID){if(tid.x<4||(tid.x<16&&MacControl.x))MacCounters[tid.x]=0;}
[numthreads(64,1,1)]
void MacClassify(uint3 tid:SV_DispatchThreadID){
    uint id=tid.x;if(id>=CoarseGrid.w)return;int3 base=int3(coarseCoord(id)*2);
    uint old=MacControl.x?0:CoarseState[id];bool eligible=MacControl.y!=0&&MacControl.w==0;
    // Immediate fine promotion at topology/contact changes. Even frozen/stale
    // importance cannot authorize a surface or solid T junction.
    [loop]for(int z=-2;z<4&&eligible;++z)[loop]for(int y=-2;y<4&&eligible;++y)
    [loop]for(int x=-2;x<4&&eligible;++x){int3 p=base+int3(x,y,z);eligible=liquid(p)&&!solid(p);
#if CUT_PRESSURE
        if(eligible)eligible=abs(cutPressureVolume(p)-1)<1e-6;
#endif
    }
    float3 lo=1e20,hi=-1e20;
    if(eligible){
        [unroll]for(uint i=0;i<8;++i){uint3 p=uint3(base)+uint3(i&1,(i>>1)&1,(i>>2)&1);
            uint3 b=(p+2)/4;
            eligible=eligible&&all(b<ImportanceGrid.xyz);
            if(all(b<ImportanceGrid.xyz))eligible=eligible&&Importance[(b.z*ImportanceGrid.y+b.y)*ImportanceGrid.x+b.x].state.x>=1;
            float3 v;[unroll]for(uint a=0;a<3;++a){uint3 q=p;q[a]++;v[a]=.5*(Faces[faceIndex(p,a)].x+Faces[faceIndex(q,a)].x);}
            lo=min(lo,v);hi=max(hi,v);
        }
        // A bounded reconstruction-error policy; promotion reacts immediately,
        // demotion requires eight safe substeps. Thresholds differ for hysteresis.
        eligible=eligible&&length(hi-lo)<((old&1)?.08:.04);
    }
    uint age=eligible?min((old>>8)+1,255u):0;
    uint next=(age<<8)|(eligible&&((old&1)||age>=8)?1u:0u);
    if((old&1)&&!(next&1))InterlockedAdd(MacCounters[4],1);
    if(!(old&1)&&(next&1))InterlockedAdd(MacCounters[5],1);
    CoarseState[id]=next;
}
[numthreads(128,1,1)]
void MacLeaves(uint3 tid:SV_DispatchThreadID){
    uint id=tid.x;if(id>=Grid.w)return;uint3 p=cellFromIndex(id);
    uint width=leafWidth(p),representative=width==2?cellIndex((p/2)*2):id;
    LeafMap[id]=representative;
    if(Cells[id].z!=1||id!=representative)return;
    uint slot;InterlockedAdd(MacCounters[0],1,slot);ActiveLeaves[slot]=id;
    if(width==2)InterlockedAdd(MacCounters[1],1);
}
[numthreads(1,1,1)]
void MacPrepare(){MacArguments[0]=(MacCounters[0]+127)/128;MacArguments[1]=1;MacArguments[2]=1;}
[numthreads(128,1,1)]
void MacRestrict(uint3 tid:SV_DispatchThreadID){
    // Preserve the original fine stencil when no coarse leaf is eligible.
    if(MacCounters[1]==0)return;
    uint id=tid.x;if(id>=3*faceStride())return;uint a=id/faceStride();uint3 p=faceCoord(id);
    if(!validFace(p,a))return;int3 l=int3(p);l[a]--;
    if(inGrid(l)&&inGrid(p)&&leaf(l)==leaf(p))return;
    uint width;float distance;uint3 base=patchBase(p,a,width,distance);
    if(any(p!=base))return;
    float v=0;[loop]for(uint j=0;j<width;++j)[loop]for(uint i=0;i<width;++i){
        uint3 q=base;q[(a+1)%3]+=i;q[(a+2)%3]+=j;v+=Faces[faceIndex(q,a)].x;
    }
    FaceScratch[id]=float4(v/(width*width),0,0,0);
    if(width==2&&distance==1.5)InterlockedAdd(MacCounters[2],1);
}
void addCoefficient(inout MacRow row,uint self,uint other,float value){
    if(self==other){row.diagonal+=value;return;}
    [loop]for(uint i=0;i<row.count;++i)if(row.neighbor[i]==other){row.coefficient[i]+=value;return;}
    if(row.count>=24){InterlockedAdd(MacCounters[3],1);return;}
    row.neighbor[row.count]=other;row.coefficient[row.count++]=value;
}
[numthreads(128,1,1)]
void MacAssemble(uint3 tid:SV_DispatchThreadID){
    if(MacCounters[1]==0&&MacControl.z==0&&!CUT_PRESSURE)return; // audit-only fine row assembly
    if(tid.x>=MacCounters[0])return;uint id=ActiveLeaves[tid.x];uint3 c=cellFromIndex(id);
    uint size=leafWidth(c);MacRow row=(MacRow)0;
    row.volumeUnits=float(size*size*size);
#if CUT_PRESSURE
    row.volumeUnits=0;float source=0;
    [loop]for(uint z=0;z<size;++z)[loop]for(uint y=0;y<size;++y)[loop]for(uint x=0;x<size;++x){
        int3 child=int3(c)+int3(x,y,z);row.volumeUnits+=cutPressureVolume(child);source+=cutPressureSource(child);
    }
#endif
    float scale=Solver.x*DomainMinCell.w/GravityDt.w;
#if PRECISE_MAC
    double preciseRhs=0;
    double preciseScale=double(Solver.x)*double(DomainMinCell.w)/double(GravityDt.w);
#if CUT_PRESSURE
    CutPressureRow exact=(CutPressureRow)0;bool regular=size==1;double volumeDelta=0;
    [loop]for(uint z=0;z<size;++z)[loop]for(uint y=0;y<size;++y)[loop]for(uint x=0;x<size;++x){
        float2 v=CutVolume[cellIndex(c+uint3(x,y,z))];volumeDelta+=double(v.x)-double(v.y);
    }
    double preciseSource=(CutSwept&1)?volumeDelta/(double(GravityDt.w)*double(DomainMinCell.w)*double(DomainMinCell.w)):0;
#endif
#endif
    [unroll]for(uint a=0;a<3;++a)[unroll]for(uint side=0;side<2;++side){
        uint3 p=c;p[a]+=side*size;int3 l=int3(p);l[a]--;
        uint width;float distance;uint3 base=patchBase(p,a,width,distance);
        float d=(side?1:-1)*float(size*size),v=MacCounters[1]?FaceScratch[faceIndex(base,a)].x:Faces[faceIndex(p,a)].x;
#if CUT_PRESSURE
        if(width==1)d*=cutPressureAperture(p,a);
#endif
        row.rhs-=d*v*scale;
#if PRECISE_MAC
#if CUT_PRESSURE
        double preciseD=(side?1.0:-1.0)*double(size*size);
        double preciseDistance=double(distance);
        if(width==1){preciseD*=cutPressureAperturePrecise(p,a);
            if(inGrid(l)&&inGrid(p))preciseDistance=cutPressureDistancePrecise(p,a);}
        regular=regular&&width==1;
        double conductance=(side?preciseD:-preciseD)/preciseDistance;
        exact.conductance[a*2+side]=conductance;exact.diagonal+=conductance;
        preciseRhs-=preciseD*double(v)*preciseScale;
#else
        preciseRhs-=double(d)*double(v)*preciseScale;
#endif
#endif
        if(solid(l)||solid(p))continue; // prescribed moving boundary flux only
        float factor=d/(width*width*distance);
        [loop]for(uint j=0;j<width;++j)[loop]for(uint i=0;i<width;++i){
            int3 q=int3(base);q[(a+1)%3]+=i;q[(a+2)%3]+=j;
            [unroll]for(uint lr=0;lr<2;++lr){int3 n=q;n[a]-=1-lr;float value=(lr?-1:1)*factor;
                if(liquid(n))addCoefficient(row,id,leaf(n),value);
                else {
                    row.boundaryDiagonal-=value;
                    if(Material.y>0){
                    float capillary=capillaryPressure(int3(c),n);row.rhs-=value*capillary;
#if PRECISE_MAC
#if CUT_PRESSURE
                    double preciseValue=(lr?-1.0:1.0)*preciseD/(double(width*width)*preciseDistance);
                    preciseRhs-=preciseValue*double(capillary);
#else
                    preciseRhs-=double(value)*double(capillary);
#endif
#endif
                    }
                }
            }
        }
    }
    if(!isfinite(row.rhs)||!isfinite(row.diagonal)||row.diagonal<0)InterlockedAdd(MacCounters[3],1);
    float absoluteSum=row.diagonal;
    [loop]for(uint k=0;k<row.count;++k)absoluteSum+=abs(row.coefficient[k]);
    row.step=(MacCounters[1]||CUT_PRESSURE)?(absoluteSum>0?1.8/absoluteSum:0):(row.diagonal>0?1/row.diagonal:0);
    if(MacCounters[1]==0&&!CUT_PRESSURE)row.rhs=FaceScratch[id].x;
#if CUT_PRESSURE
    row.rhs-=source*scale;
#if PRECISE_MAC
    preciseRhs-=preciseSource*preciseScale;
    exact.rhs=preciseRhs;if(!regular)exact.diagonal=-1;
    CutExact[id]=exact;
#endif
#endif
#if PRECISE_MAC
    row.rhs=float(preciseRhs);
#endif
    Rows[id]=row;
}
[numthreads(128,1,1)]
void MacSmooth(uint3 tid:SV_DispatchThreadID){
    if(tid.x>=MacCounters[0])return;uint id=ActiveLeaves[tid.x];
    if(MacCounters[1]==0&&!CUT_PRESSURE){PressureOut[id]=pressureStencil(id);return;}
    MacRow row=Rows[id];float ap=row.diagonal*PressureIn[id];
    [loop]for(uint i=0;i<row.count;++i)ap+=row.coefficient[i]*PressureIn[row.neighbor[i]];
    // H=diag(sum |Aij|) gives lambda(H^-1 A)<=1. A is PSD, hence
    // 0<omega=1.8<2 is stable, including positive T-junction couplings.
    // Uniform interior cells get 0.9/diagonal, not a global 1/3 penalty.
    PressureOut[id]=PressureIn[id]+(row.rhs-ap)*row.step;
}
struct MacDebugVertex {float4 position:SV_Position;float3 colour:COLOR0;};
MacDebugVertex MacVS(uint vertex:SV_VertexID,uint instance:SV_InstanceID){
    MacDebugVertex o;o.position=float4(2,2,2,1);o.colour=0;
    uint3 p=cellFromIndex(instance);
    if(Cells[instance].z!=1||LeafMap[instance]!=instance)return o;
    uint size=leafWidth(p);bool boundary=false;
    if(size==1)[unroll]for(uint a=0;a<3;++a)[unroll]for(int s=-1;s<=1;s+=2){int3 q=int3(p);q[a]+=s;boundary=boundary||leafWidth(q)==2;}
    if(size==1&&!boundary)return o;
    uint edge=vertex/2,a=edge/4,j=edge%4;float3 corner=0;
    corner[a]=vertex&1;corner[(a+1)%3]=j&1;corner[(a+2)%3]=j>>1;
    float3 world=DomainMinCell.xyz+(float3(p)+corner*size)*DomainMinCell.w;
    o.position=mul(float4(world,1),ViewProjection);o.colour=size==2?float3(.1,1,.45):float3(1,.55,.05);return o;
}
float4 MacPS(MacDebugVertex v):SV_Target{return float4(v.colour,1);}
[numthreads(128,1,1)]
void MacMeasure(uint3 tid:SV_DispatchThreadID){
    uint id=tid.x;if(id>=Grid.w)return;uint3 p=cellFromIndex(id);float divergence=0;
#if CUT_PRESSURE
    double flux=0;
    [unroll]for(uint a=0;a<3;++a){uint3 q=p;q[a]++;flux+=CutFlux[faceIndex(q,a)]-CutFlux[faceIndex(p,a)];}
    float2 volume=CutVolume[id];if(CutSwept&1)flux+=(double(volume.x)-double(volume.y))/double(GravityDt.w);
    float4 c=Cells[id];c.y=c.z==1?float(flux/double(cutPressureCapacity(p))):0;
#else
    [unroll]for(uint a=0;a<3;++a){uint3 q=p;q[a]++;divergence+=Faces[faceIndex(q,a)].x-Faces[faceIndex(p,a)].x;}
    float4 c=Cells[id];c.y=c.z==1?divergence/DomainMinCell.w:0;
#endif
    c.w=PressureIn[LeafMap[id]];Cells[id]=c; // display the actual leaf pressure, not empty child slots
}
[numthreads(128,1,1)]
void MacProject(uint3 tid:SV_DispatchThreadID){
    uint id=tid.x;if(id>=3*faceStride())return;uint a=id/faceStride();uint3 p=faceCoord(id);
    if(!validFace(p,a))return;int3 l=int3(p);l[a]--;
    if(inGrid(l)&&inGrid(p)&&leaf(l)==leaf(p))return;
    float4 f=Faces[id];
#if CUT_PRESSURE
    double exactVelocity=double(f.x);
    // Forces/viscosity already supplied the solid extension velocity. Closed
    // faces carry no liquid flux and receive no pressure correction.
    if(cutPressureAperture(p,a)==0){f.w=1;}
#else
    if(solid(l)||solid(p)){f.x=boundaryVelocity(l,p,a);f.w=1;}
#endif
    else if(liquid(l)||liquid(p)){
#if PRECISE_MAC
        uint width;float distance;uint3 base=patchBase(p,a,width,distance);double dp=0;
        [loop]for(uint j=0;j<width;++j)[loop]for(uint i=0;i<width;++i){
            int3 q=int3(base);q[(a+1)%3]+=i;q[(a+2)%3]+=j;
            [unroll]for(uint lr=0;lr<2;++lr){int3 n=q;n[a]-=1-lr;
                double pressure=liquid(n)?PrecisePressure[leaf(n)]:0;
                if(!liquid(n)&&Material.y>0)pressure=double(capillaryPressure(liquid(l)?l:int3(p),n));
                dp+=(lr?1:-1)*pressure;
            }
        }
        float before=MacCounters[1]?FaceScratch[faceIndex(base,a)].x:f.x;
        double k=double(GravityDt.w)/(double(Solver.x)*double(DomainMinCell.w));
        double preciseDistance=double(distance);
#if CUT_PRESSURE
        if(width==1)preciseDistance=cutPressureDistancePrecise(p,a);
#endif
        double projected=double(before)-k*dp/(double(width*width)*preciseDistance);
        f.x=float(projected);f.w=1;
#if CUT_PRESSURE
        exactVelocity=projected;
#endif
#else
        if(MacCounters[1]==0&&!CUT_PRESSURE){
            float pl=PressureIn[cellIndex(l)],pr=PressureIn[cellIndex(p)];
            if(Material.y>0){if(!liquid(l))pl=capillaryPressure(p,l);if(!liquid(p))pr=capillaryPressure(l,p);}
            f.x-=GravityDt.w/(Solver.x*DomainMinCell.w)*(pr-pl);f.w=1;Faces[id]=f;return;
        }
        uint width;float distance;uint3 base=patchBase(p,a,width,distance);float dp=0;
        [loop]for(uint j=0;j<width;++j)[loop]for(uint i=0;i<width;++i){
            int3 q=int3(base);q[(a+1)%3]+=i;q[(a+2)%3]+=j;
            [unroll]for(uint lr=0;lr<2;++lr){int3 n=q;n[a]-=1-lr;
                float pressure=liquid(n)?PressureIn[leaf(n)]:0;
                if(!liquid(n)&&Material.y>0)pressure=capillaryPressure(liquid(l)?l:int3(p),n);
                dp+=(lr?1:-1)*pressure;
            }
        }
        float before=MacCounters[1]?FaceScratch[faceIndex(base,a)].x:f.x;
        f.x=before-GravityDt.w/(Solver.x*DomainMinCell.w)*dp/(width*width*distance);f.w=1;
#endif
    }else f.w=0;
#if CUT_PRESSURE
#if !PRECISE_MAC
    exactVelocity=double(f.x);
#endif
    CutFlux[id]=cutPressureAperture(p,a)>0?exactVelocity*double(CutAperture[id]):0;
#endif
    Faces[id]=f;
}
float parity(uint i){return (countbits(i)&1)?-1:1;}
#if CUT_PRESSURE
[numthreads(128,1,1)]
void MacApplyCapacity(uint id:SV_DispatchThreadID){
    if(id>=3*faceStride())return;uint a=id/faceStride();uint3 p=faceCoord(id);
    if(!validFace(p,a)||cutPressureAperture(p,a)<=0)return;
    double next=CapacityFlux[id];
    if(next!=CutFlux[id]){
        float4 f=Faces[id];f.x=float(next/double(CutAperture[id]));f.w=1;Faces[id]=f;
    }
    CutFlux[id]=next;
}
#endif
[numthreads(64,1,1)]
void MacProlongate(uint3 tid:SV_DispatchThreadID){
    uint id=tid.x;if(id>=CoarseGrid.w||!(CoarseState[id]&1))return;uint3 base=coarseCoord(id)*2;
    float external[24],internal[12],div[8],phi[8];
    [unroll]for(uint a=0;a<3;++a)[unroll]for(uint j=0;j<4;++j){
        uint3 p=base;p[(a+1)%3]+=j&1;p[(a+2)%3]+=j>>1;
        external[a*8+j]=Faces[faceIndex(p,a)].x;p[a]+=2;external[a*8+4+j]=Faces[faceIndex(p,a)].x;
        internal[a*4+j]=.5*(external[a*8+j]+external[a*8+4+j]);
    }
    float mean=0;
    [unroll]for(uint i=0;i<8;++i){uint3 p=uint3(i&1,(i>>1)&1,(i>>2)&1);div[i]=0;phi[i]=0;
        [unroll]for(uint a=0;a<3;++a){uint j=p[(a+1)%3]+2*p[(a+2)%3];
            div[i]+=p[a]?external[a*8+4+j]-internal[a*4+j]:internal[a*4+j]-external[a*8+j];}
        mean+=div[i]/8;
    }
    // Exact eight-cell Neumann solve in the Walsh basis: eigenvalues 2*popcount.
    [unroll]for(uint mode=1;mode<8;++mode){float coefficient=0;
        [unroll]for(uint i=0;i<8;++i)coefficient+=parity(i&mode)*(mean-div[i])/(16*countbits(mode));
        [unroll]for(uint k=0;k<8;++k)phi[k]+=parity(k&mode)*coefficient;
    }
    [unroll]for(uint a=0;a<3;++a)[unroll]for(uint j=0;j<4;++j){
        uint3 p=0;p[(a+1)%3]=j&1;p[(a+2)%3]=j>>1;uint left=p.x+2*p.y+4*p.z;
        float v=internal[a*4+j]-phi[left+(1u<<a)]+phi[left];p+=base;p[a]++;
        uint f=faceIndex(p,a);float4 value=Faces[f];value.x=v;value.w=1;Faces[f]=value;
#if CUT_PRESSURE
        // Internal prolongation is a transfer cache, not another coarse DOF.
        CutFlux[f]=double(v)*double(CutAperture[f]);
#endif
    }
}
