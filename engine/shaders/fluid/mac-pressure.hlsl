#include "mac-row.hlsli"
#ifndef CUT_PRESSURE
#define CUT_PRESSURE 0
#endif
#if CUT_PRESSURE
#include "cut-pressure-row.hlsli"
RWStructuredBuffer<CutPressureRow> CutExact:register(u20);
#endif
cbuffer HierarchyFrame:register(b0){uint4 FineGrid;uint4 Levels[16];uint4 Control;float4 Parameters;}
cbuffer HierarchyStage:register(b1){uint4 Stage;} // level, CG iteration, reduction mode, reserved
RWStructuredBuffer<MacRow> BaseRows:register(u0);
RWStructuredBuffer<uint> BaseMap:register(u1);
RWStructuredBuffer<float4> BaseCells:register(u2);
RWStructuredBuffer<uint> BaseList:register(u3);
RWStructuredBuffer<uint> BaseCounts:register(u4);
RWStructuredBuffer<float> Solution:register(u5);
RWStructuredBuffer<float> FineIn:register(u6);
RWStructuredBuffer<float> FineOut:register(u7);
RWStructuredBuffer<float4> Vectors:register(u8); // residual, preconditioned residual, direction, A direction
struct HierarchyRow{float4 xy;float4 z;}; // six positive face conductances, diagonal, air-boundary diagonal
RWStructuredBuffer<HierarchyRow> HRows:register(u9);
RWStructuredBuffer<float> HRhs:register(u10);
RWStructuredBuffer<float> HIn:register(u11);
RWStructuredBuffer<float> HOut:register(u12);
RWStructuredBuffer<double> PrecisePressure:register(u13);
RWStructuredBuffer<uint> Work:register(u14);
RWStructuredBuffer<uint> HCounts:register(u15); // [0] base, [1..16] coarse; [20] solving, [21] invalid, [22] iterations
RWStructuredBuffer<uint> Arguments:register(u16);
RWStructuredBuffer<float4> Partials:register(u17);
RWStructuredBuffer<float4> Scalars:register(u18); // [0] initial/final norm2 and max divergence; [1] rz,beta,alpha,reserved
RWStructuredBuffer<float> Factor:register(u19);
RWStructuredBuffer<double> ExternalCorrection:register(u21);
groupshared float4 Reduction[128];
uint product(uint3 d){return d.x*d.y*d.z;}
uint3 coord(uint id,uint3 d){return uint3(id%d.x,(id/d.x)%d.y,id/(d.x*d.y));}
uint index(uint3 p,uint3 d){return (p.z*d.y+p.y)*d.x+p.x;}
bool inside(int3 p,uint3 d){return all(p>=0)&&all(p<int3(d));}
float weight(HierarchyRow r,uint s){return s<4?r.xy[s]:r.z[s-4];}
void setWeight(inout HierarchyRow r,uint s,float v){if(s<4)r.xy[s]=v;else r.z[s-4]=v;}
float leafVolume(uint id){return BaseRows[id].volumeUnits;}
#if CUT_PRESSURE
double exactApply(uint id,uint source){
    double p=source?double(Vectors[id].z):PrecisePressure[id],ap=0;
    if(CutExact[id].diagonal<0){
        ap=double(BaseRows[id].diagonal)*p;
        [loop]for(uint j=0;j<BaseRows[id].count;++j){uint n=BaseRows[id].neighbor[j];
            ap+=double(BaseRows[id].coefficient[j])*(source?double(Vectors[n].z):PrecisePressure[n]);}
    }else{
        int3 c=int3(coord(id,FineGrid.xyz));
        [unroll]for(uint a=0;a<3;++a)[unroll]for(uint s=0;s<2;++s){
            double w=CutExact[id].conductance[a*2+s];if(w==0)continue;
            int3 q=c;q[a]+=s?1:-1;double neighbor=0;
            if(inside(q,FineGrid.xyz)){uint n=index(q,FineGrid.xyz);
                if(BaseCells[n].z==1)neighbor=source?double(Vectors[n].z):PrecisePressure[n];}
            ap+=w*(p-neighbor);
        }
    }
    return ap;
}
#endif
float4 reduceGroup(float4 v,uint lane){
    Reduction[lane]=v;GroupMemoryBarrierWithGroupSync();
    [unroll]for(uint n=64;n;n>>=1){if(lane<n){float4 b=Reduction[lane+n];Reduction[lane]=float4(Reduction[lane].xyz+b.xyz,max(Reduction[lane].w,b.w));}GroupMemoryBarrierWithGroupSync();}
    return Reduction[0];
}
float baseApply(uint id,uint source){
#if CUT_PRESSURE
    if(source==1)return float(exactApply(id,1));
#endif
    // Index the structured buffer directly. Copying MacRow into a local makes
    // DXC materialize two dynamically indexed 24-element private arrays and
    // eagerly load unused entries on every smoother/matrix application.
    float v=BaseRows[id].diagonal*(source==0?FineIn[id]:source==1?Vectors[id].z:Solution[id]);
    uint count=BaseRows[id].count;
    [loop]for(uint j=0;j<count;++j){uint n=BaseRows[id].neighbor[j];v+=BaseRows[id].coefficient[j]*(source==0?FineIn[n]:source==1?Vectors[n].z:Solution[n]);}return v;
}
float coarseValue(uint at,uint source){return source?HOut[at]:HIn[at];}
float coarseApplyFrom(uint id,uint level,uint source){
    uint4 g=Levels[level];uint3 p=coord(id,g.xyz);uint at=g.w+id;HierarchyRow r=HRows[at];float v=r.z.z*coarseValue(at,source);
    [unroll]for(uint a=0;a<3;++a)[unroll]for(uint s=0;s<2;++s){float w=weight(r,a*2+s);if(w>0){int3 q=int3(p);q[a]+=s?1:-1;v-=w*coarseValue(g.w+index(q,g.xyz),source);}}
    return v;
}
float coarseApply(uint id,uint level){return coarseApplyFrom(id,level,0);}
[numthreads(128,1,1)]void MgClear(uint3 tid:SV_DispatchThreadID){if(tid.x<64)HCounts[tid.x]=0;if(tid.x<4)Scalars[tid.x]=0;}
[numthreads(128,1,1)]void MgAssemble(uint3 tid:SV_DispatchThreadID){
    uint l=Stage.x;uint4 g=Levels[l];uint id=tid.x;if(id>=product(g.xyz))return;
    uint3 p=coord(id,g.xyz),base=p*(l?2:4);HierarchyRow outRow=(HierarchyRow)0;float ghost=0;
    uint extent=l?2:4;uint3 childGrid=l?Levels[l-1].xyz:FineGrid.xyz;
    [loop]for(uint z=0;z<extent;++z)[loop]for(uint y=0;y<extent;++y)[loop]for(uint x=0;x<extent;++x){
        uint3 q=base+uint3(x,y,z);if(any(q>=childGrid))continue;uint child=index(q,childGrid);
        if(l==0){
            if(BaseCells[child].z!=1||BaseMap[child]!=child)continue;uint count=BaseRows[child].count;
            ghost+=BaseRows[child].boundaryDiagonal;
            if(!(BaseRows[child].volumeUnits>0)||!isfinite(BaseRows[child].volumeUnits)||
               !(BaseRows[child].boundaryDiagonal>=0)||!isfinite(BaseRows[child].boundaryDiagonal))InterlockedAdd(HCounts[21],1);
            [loop]for(uint j=0;j<count;++j){int3 parent=int3(coord(BaseRows[child].neighbor[j],FineGrid.xyz)/4),delta=parent-int3(p);if(all(delta==0))continue;
                if(dot(abs(delta),int3(1,1,1))!=1){InterlockedAdd(HCounts[21],1);continue;}
                uint axis=delta.x!=0?0:delta.y!=0?1:2,side=axis*2+(delta[axis]>0?1:0);
                setWeight(outRow,side,weight(outRow,side)-BaseRows[child].coefficient[j]);
            }
        }else{
            HierarchyRow r=HRows[Levels[l-1].w+child];ghost+=r.z.w;
            [unroll]for(uint a=0;a<3;++a)[unroll]for(uint s=0;s<2;++s){int3 n=int3(q);n[a]+=s?1:-1;
                if(inside(n,childGrid)&&any(uint3(n)/2!=p))setWeight(outRow,a*2+s,weight(outRow,a*2+s)+weight(r,a*2+s));
            }
        }
    }
    outRow.z.w=ghost;HRows[g.w+id]=outRow;HRhs[g.w+id]=0;HIn[g.w+id]=0;HOut[g.w+id]=0;
}
[numthreads(128,1,1)]void MgCanonicalFaces(uint3 tid:SV_DispatchThreadID){
    uint l=Stage.x;uint4 g=Levels[l];uint id=tid.x;if(id>=product(g.xyz))return;uint3 p=coord(id,g.xyz);uint at=g.w+id;
    HierarchyRow r=HRows[at];float diagonal=r.z.w;
    [unroll]for(uint a=0;a<3;++a){float negative=0;if(p[a]){uint3 q=p;q[a]--;negative=weight(HRows[g.w+index(q,g.xyz)],a*2+1);}
        // Never rewrite positive faces here: neighboring threads read those
        // immutable components to obtain bit-identical shared conductances.
        if(a==0)HRows[at].xy.x=negative;else if(a==1)HRows[at].xy.z=negative;else HRows[at].z.x=negative;
        diagonal+=negative+weight(r,a*2+1);
    }
    HRows[at].z.z=diagonal;
    if(!isfinite(diagonal)||diagonal<0)InterlockedAdd(HCounts[21],1);
    if(diagonal>0){uint slot;InterlockedAdd(HCounts[l+1],1,slot);Work[g.w+slot]=id;}
}
[numthreads(1,1,1)]void MgPrepare(){
    HCounts[0]=BaseCounts[0];HCounts[20]=1;
    for(uint l=0;l<=Control.x;++l){Arguments[l*3]=(HCounts[l]+127)/128;Arguments[l*3+1]=1;Arguments[l*3+2]=1;}
    Arguments[(Control.x+1)*3]=1;Arguments[(Control.x+1)*3+1]=1;Arguments[(Control.x+1)*3+2]=1;
    Arguments[(Control.x+2)*3]=(FineGrid.w+127)/128;Arguments[(Control.x+2)*3+1]=1;Arguments[(Control.x+2)*3+2]=1;
    Arguments[62]=1;Arguments[63]=0; // aligned 64-bit DX12 execution predicate
}
groupshared float Dense[4096];
groupshared float BottomVector[64];
[numthreads(128,1,1)]void MgFactor(uint lane:SV_GroupIndex){
    uint4 g=Levels[Control.x-1];uint n=product(g.xyz);float scale=lane<n?HRows[g.w+lane].z.z:0;
    float4 maximum=reduceGroup(float4(0,0,0,scale),lane);float shift=max(1,maximum.w)*Parameters.w;
    for(uint t=lane;t<4096;t+=128){uint i=t/64,j=t%64;float v=0;
        if(i<n&&j<n){HierarchyRow r=HRows[g.w+i];if(i==j)v=r.z.z+shift;
            else{int3 d=int3(coord(j,g.xyz))-int3(coord(i,g.xyz));if(dot(abs(d),int3(1,1,1))==1){uint a=d.x!=0?0:d.y!=0?1:2;v=-weight(r,a*2+(d[a]>0?1:0));}}
        }Dense[t]=v;
    }GroupMemoryBarrierWithGroupSync();
    [loop]for(uint k=0;k<n;++k){
        if(lane==k){float v=Dense[k*64+k];for(uint j=0;j<k;++j)v-=Dense[k*64+j]*Dense[k*64+j];
            if(!(v>0)||!isfinite(v)){InterlockedAdd(HCounts[21],1);v=1;}Dense[k*64+k]=sqrt(v);}
        GroupMemoryBarrierWithGroupSync();
        if(lane>k&&lane<n){float v=Dense[lane*64+k];for(uint j=0;j<k;++j)v-=Dense[lane*64+j]*Dense[k*64+j];Dense[lane*64+k]=v/Dense[k*64+k];}
        GroupMemoryBarrierWithGroupSync();
    }
    for(uint t=lane;t<4096;t+=128)Factor[t]=Dense[t];
}
[numthreads(128,1,1)]void MgInitialize(uint3 tid:SV_DispatchThreadID,uint lane:SV_GroupIndex,uint3 group:SV_GroupID){
    // Dot products use stable geometric order, not nondeterministic atomic
    // append order. Iterative smoothing still uses the compact active list.
    float4 v=0;uint id=tid.x;if(id<FineGrid.w&&BaseCells[id].z==1&&BaseMap[id]==id){float r=BaseRows[id].rhs;
        Solution[id]=0;PrecisePressure[id]=0;Vectors[id]=float4(r,0,0,0);v=float4(0,0,r*r,abs(r)*Parameters.z/leafVolume(id));}
    float4 sum=reduceGroup(v,lane);if(lane==0)Partials[group.x]=sum;
}
[numthreads(128,1,1)]void MgResetCorrection(uint3 tid:SV_DispatchThreadID){if(tid.x>=HCounts[0])return;uint id=BaseList[tid.x];FineIn[id]=0;FineOut[id]=0;}
[numthreads(128,1,1)]void MgImportCorrection(uint id:SV_DispatchThreadID){
    if(id>=FineGrid.w)return;
    Vectors[id]=float4(BaseCells[id].z==1&&BaseMap[id]==id?float(ExternalCorrection[id]):0,0,0,0);
}
[numthreads(128,1,1)]void MgExportCorrection(uint id:SV_DispatchThreadID){
    if(id>=FineGrid.w)return;
    ExternalCorrection[id]=BaseCells[id].z==1&&BaseMap[id]==id?double(FineIn[id]):0;
}
[numthreads(128,1,1)]void MgFineSmooth(uint3 tid:SV_DispatchThreadID){if(tid.x>=HCounts[0])return;uint id=BaseList[tid.x];FineOut[id]=FineIn[id]+.8*BaseRows[id].step*(Vectors[id].x-baseApply(id,0));}
[numthreads(128,1,1)]void MgRestrictBase(uint3 tid:SV_DispatchThreadID){
    uint4 g=Levels[0];if(tid.x>=HCounts[1])return;uint id=Work[g.w+tid.x];uint3 base=coord(id,g.xyz)*4;float rhs=0;
    [loop]for(uint z=0;z<4;++z)[loop]for(uint y=0;y<4;++y)[loop]for(uint x=0;x<4;++x){uint3 q=base+uint3(x,y,z);if(any(q>=FineGrid.xyz))continue;
        uint child=index(q,FineGrid.xyz);if(BaseCells[child].z==1&&BaseMap[child]==child)rhs+=Vectors[child].x-baseApply(child,0);}
    HRhs[g.w+id]=rhs;HIn[g.w+id]=0;HOut[g.w+id]=0;
}
[numthreads(128,1,1)]void MgCoarseSmooth(uint3 tid:SV_DispatchThreadID){uint l=Stage.x;uint4 g=Levels[l];if(tid.x>=HCounts[l+1])return;uint id=Work[g.w+tid.x],at=g.w+id;HOut[at]=HIn[at]+(2.0/3)*(HRhs[at]-coarseApply(id,l))/HRows[at].z.z;}
[numthreads(128,1,1)]void MgRestrictCoarse(uint3 tid:SV_DispatchThreadID){
    uint l=Stage.x;uint4 g=Levels[l],childGrid=Levels[l-1];if(tid.x>=HCounts[l+1])return;uint id=Work[g.w+tid.x];uint3 base=coord(id,g.xyz)*2;float rhs=0;
    [unroll]for(uint i=0;i<8;++i){uint3 q=base+uint3(i&1,(i>>1)&1,(i>>2)&1);if(any(q>=childGrid.xyz))continue;uint child=index(q,childGrid.xyz);rhs+=HRhs[childGrid.w+child]-coarseApply(child,l-1);}
    HRhs[g.w+id]=rhs;HIn[g.w+id]=0;HOut[g.w+id]=0;
}
void solveBottom(uint lane){
    uint4 g=Levels[Control.x-1];uint n=product(g.xyz);if(lane<n)BottomVector[lane]=HRhs[g.w+lane];GroupMemoryBarrierWithGroupSync();
    [loop]for(uint i=0;i<n;++i){if(lane==i)BottomVector[i]/=Factor[i*64+i];GroupMemoryBarrierWithGroupSync();if(lane>i&&lane<n)BottomVector[lane]-=Factor[lane*64+i]*BottomVector[i];GroupMemoryBarrierWithGroupSync();}
    [loop]for(int k=int(n)-1;k>=0;--k){if(lane==uint(k))BottomVector[k]/=Factor[k*64+k];GroupMemoryBarrierWithGroupSync();if(lane<uint(k))BottomVector[lane]-=Factor[k*64+lane]*BottomVector[k];GroupMemoryBarrierWithGroupSync();}
    if(lane<n)HIn[g.w+lane]=BottomVector[lane];
}
[numthreads(128,1,1)]void MgBottom(uint lane:SV_GroupIndex){solveBottom(lane);}
// Small correction hierarchies fit efficiently in one thread group's work.
// Global buffer storage is retained; device+group barriers order all dependent
// stages. There is no cross-group spin barrier and no persistent-grid deadlock.
void smoothCoarseGroup(uint l,uint lane){
    uint4 g=Levels[l];
    for(uint i=lane;i<HCounts[l+1];i+=128){uint id=Work[g.w+i],at=g.w+id;
        HOut[at]=HIn[at]+(2.0/3)*(HRhs[at]-coarseApplyFrom(id,l,0))/HRows[at].z.z;
    }
    AllMemoryBarrierWithGroupSync();
    for(uint i=lane;i<HCounts[l+1];i+=128){uint id=Work[g.w+i],at=g.w+id;
        HIn[at]=HOut[at]+(2.0/3)*(HRhs[at]-coarseApplyFrom(id,l,1))/HRows[at].z.z;
    }
    AllMemoryBarrierWithGroupSync();
}
[numthreads(128,1,1)]void MgCoarseCycle(uint lane:SV_GroupIndex){
    for(uint l=0;l+1<Control.x;++l){
        smoothCoarseGroup(l,lane);
        uint4 g=Levels[l+1],childGrid=Levels[l];
        for(uint work=lane;work<HCounts[l+2];work+=128){uint id=Work[g.w+work];uint3 base=coord(id,g.xyz)*2;float rhs=0;
            [unroll]for(uint i=0;i<8;++i){uint3 q=base+uint3(i&1,(i>>1)&1,(i>>2)&1);if(any(q>=childGrid.xyz))continue;uint child=index(q,childGrid.xyz);rhs+=HRhs[childGrid.w+child]-coarseApply(child,l);}
            HRhs[g.w+id]=rhs;HIn[g.w+id]=0;HOut[g.w+id]=0;
        }
        AllMemoryBarrierWithGroupSync();
    }
    solveBottom(lane);
    AllMemoryBarrierWithGroupSync();
    for(int level=int(Control.x)-2;level>=0;--level){
        uint l=uint(level);uint4 g=Levels[l],parent=Levels[l+1];
        for(uint work=lane;work<HCounts[l+1];work+=128){uint id=Work[g.w+work];HIn[g.w+id]+=HIn[parent.w+index(coord(id,g.xyz)/2,parent.xyz)];}
        AllMemoryBarrierWithGroupSync();
        smoothCoarseGroup(l,lane);
    }
}
[numthreads(128,1,1)]void MgProlongCoarse(uint3 tid:SV_DispatchThreadID){uint l=Stage.x;uint4 g=Levels[l],parent=Levels[l+1];if(tid.x>=HCounts[l+1])return;uint id=Work[g.w+tid.x];HIn[g.w+id]+=HIn[parent.w+index(coord(id,g.xyz)/2,parent.xyz)];}
[numthreads(128,1,1)]void MgProlongFine(uint3 tid:SV_DispatchThreadID){if(tid.x>=HCounts[0])return;uint id=BaseList[tid.x];FineIn[id]+=HIn[Levels[0].w+index(coord(id,FineGrid.xyz)/4,Levels[0].xyz)];}
[numthreads(128,1,1)]void MgDotPreconditioned(uint3 tid:SV_DispatchThreadID,uint lane:SV_GroupIndex,uint3 group:SV_GroupID){
    float4 v=0;uint id=tid.x;if(id<FineGrid.w&&BaseCells[id].z==1&&BaseMap[id]==id){float4 q=Vectors[id];q.y=FineIn[id];Vectors[id]=q;v.x=q.x*q.y;}
    float4 sum=reduceGroup(v,lane);if(lane==0)Partials[group.x]=sum;
}
[numthreads(128,1,1)]void MgDirection(uint3 tid:SV_DispatchThreadID){if(tid.x>=HCounts[0])return;uint id=BaseList[tid.x];float4 v=Vectors[id];v.z=v.y+Scalars[1].y*v.z;Vectors[id]=v;}
[numthreads(128,1,1)]void MgApply(uint3 tid:SV_DispatchThreadID,uint lane:SV_GroupIndex,uint3 group:SV_GroupID){
    float4 v=0;uint id=tid.x;if(id<FineGrid.w&&BaseCells[id].z==1&&BaseMap[id]==id){float ad=baseApply(id,1);Vectors[id].w=ad;v.y=Vectors[id].z*ad;}
    float4 sum=reduceGroup(v,lane);if(lane==0)Partials[group.x]=sum;
}
[numthreads(128,1,1)]void MgUpdate(uint3 tid:SV_DispatchThreadID){
    if(tid.x>=HCounts[0])return;uint id=BaseList[tid.x];
    double p=PrecisePressure[id]+double(Scalars[1].z)*double(Vectors[id].z);
    PrecisePressure[id]=p;Solution[id]=float(p); // float cache is display/legacy output only
}
float preciseResidual(uint id){
#if CUT_PRESSURE
    return float(CutExact[id].rhs-exactApply(id,0));
#else
    double r=double(BaseRows[id].rhs)-double(BaseRows[id].diagonal)*PrecisePressure[id];
    uint count=BaseRows[id].count;
    [loop]for(uint j=0;j<count;++j)r-=double(BaseRows[id].coefficient[j])*PrecisePressure[BaseRows[id].neighbor[j]];
    return float(r);
#endif
}
[numthreads(128,1,1)]void MgResidual(uint3 tid:SV_DispatchThreadID,uint lane:SV_GroupIndex,uint3 group:SV_GroupID){
    float4 v=0;uint id=tid.x;if(id<FineGrid.w&&BaseCells[id].z==1&&BaseMap[id]==id){float4 q=Vectors[id];q.x=preciseResidual(id);Vectors[id]=q;v=float4(0,0,q.x*q.x,abs(q.x)*Parameters.z/leafVolume(id));}
    float4 sum=reduceGroup(v,lane);if(lane==0)Partials[group.x]=sum;
}
[numthreads(128,1,1)]void MgReduce(uint lane:SV_GroupIndex){
    // Uniform across this entire group. Do not read/reduce stale partials after
    // convergence; the reserved indirect work has already been set to zero.
    if(HCounts[20]==0&&Stage.z!=0)return;
    float4 sum=0;for(uint i=lane;i<(FineGrid.w+127)/128;i+=128){float4 v=Partials[i];sum=float4(sum.xyz+v.xyz,max(sum.w,v.w));}
    sum=reduceGroup(sum,lane);if(lane)return;
    if(!all(isfinite(sum))){InterlockedAdd(HCounts[21],1);HCounts[20]=0;}
    if(Stage.z==0||Stage.z==3){
        float4 s=Scalars[0];if(Stage.z==0)s.xy=float2(sum.z,sum.w);else HCounts[22]++;
        s.zw=float2(sum.z,sum.w);Scalars[0]=s;
        if(Stage.z==3)Scalars[4+Stage.y]=float4(sum.z,sum.w,Scalars[1].x,Scalars[1].z);
        if(sum.z<=s.x*Parameters.x&&sum.w<=Parameters.y)HCounts[20]=0;
    }else if(Stage.z==1){float4 s=Scalars[1];s.y=Stage.y?sum.x/max(s.x,1e-30):0;s.x=sum.x;Scalars[1]=s;
        if(!(sum.x>0)){InterlockedAdd(HCounts[21],1);HCounts[20]=0;}}
    else {float4 s=Scalars[1];s.z=s.x/max(sum.y,1e-30);Scalars[1]=s;
        if(!(sum.y>0)){InterlockedAdd(HCounts[21],1);HCounts[20]=0;}}
    if(HCounts[20]==0){for(uint l=0;l<=Control.x+2;++l)Arguments[l*3]=0;Arguments[62]=0;}
}
