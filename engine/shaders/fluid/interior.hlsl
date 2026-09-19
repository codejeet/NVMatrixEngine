#include "common.hlsli"
cbuffer InteriorFrame : register(b1){uint4 InteriorGrid,ImportanceGrid,InteriorControl;float4 InteriorPolicy;};
RWStructuredBuffer<uint> InteriorFree : register(u24);
RWStructuredBuffer<uint> InteriorActive : register(u27);
RWByteAddressBuffer InteriorArguments : register(u25);
struct ImportanceBrick {float4 importance,dynamics,velocity;uint4 state;};
RWStructuredBuffer<ImportanceBrick> InteriorImportance : register(u26);
float3 coarseCenter(uint id){return DomainMinCell.xyz+(float3(interiorCoord(id,Grid.xyz))*2+1)*DomainMinCell.w;}
float affineSquared(FluidParticle p){return dot(p.apic0.xyz,p.apic0.xyz)+dot(p.apic1.xyz,p.apic1.xyz)+dot(p.apic2.xyz,p.apic2.xyz);}
float3x3 outer(float3 v,float3 x){return float3x3(v.x*x,v.y*x,v.z*x);}
uint demandedLod(uint id){if(!InteriorControl.y)return 0;
    uint3 b=(interiorCoord(id,Grid.xyz)*2+2)/4;
    return InteriorImportance[(b.z*ImportanceGrid.y+b.y)*ImportanceGrid.x+b.x].state.x;
}
bool safeSolid(uint id){float3 c=coarseCenter(id);float radius=length(float3(1,1,1))*DomainMinCell.w+3*DomainMinCell.w;
    for(uint i=0;i<Collision.x;++i)if(colliderPhi(Colliders[i],c,radius)<radius)return false;
    return true;
}
bool occupiedByParticles(uint id){uint3 base=interiorCoord(id,Grid.xyz)*2;
    [unroll]for(uint child=0;child<8;++child){uint3 p=base+uint3(child&1,(child>>1)&1,child>>2);
        if(all(p<Grid.xyz)){uint c=cellIndex(p);if(CellOffsets[c+1]>CellOffsets[c])return true;}}
    return false;
}
[numthreads(64,1,1)]void InteriorBegin(uint id:SV_DispatchThreadID){
    if(id<16)InteriorTotals[id]=0;
    if(id<InteriorGrid.w&&InteriorControl.z)Interior[id]=(InteriorCell)0;
}
[numthreads(16,1,1)]void InteriorClearExchange(uint id:SV_DispatchThreadID){
    if(id==0||id==1||id==12)InteriorTotals[id]=0;
}
[numthreads(256,1,1)]void InteriorFreeSlots(uint id:SV_DispatchThreadID){
    if(id>=InteriorControl.x||Particles[id].velocityFlags.w)return;
    uint slot;InterlockedAdd(InteriorTotals[4],1,slot);InteriorFree[slot]=id;
}
// Bounded validation/reference mode only: stable physical-sample IDs through
// ownership changes. Sorting bins by ID cannot order an unordered allocator.
[numthreads(1,1,1)]void InteriorFreeSlotsOrdered(){
    uint count=0;
    for(uint id=0;id<InteriorControl.x;++id)if(!Particles[id].velocityFlags.w)InteriorFree[count++]=id;
    InteriorTotals[4]=count;
}
void restoreInterior(uint id){
    if(id>=InteriorGrid.w)return;InteriorCell c=Interior[id];if(!c.velocityFlags.w)return;
    if(InteriorPolicy.w==0)return;
    bool wake=InteriorPolicy.z!=0||demandedLod(id)==0||!safeSolid(id)||occupiedByParticles(id);
    float wakeSpeed=min(InteriorPolicy.y,1e-4); // restore as soon as nontrivial global-pressure flow arrives
    wake|=length(c.velocityFlags.xyz)>wakeSpeed||any(abs(c.positionRadius.xyz-coarseCenter(id))>.30*DomainMinCell.w);
    float3x3 C=float3x3(c.apic0.xyz,c.apic1.xyz,c.apic2.xyz);
    wake|=sqrt(dot(C[0],C[0])+dot(C[1],C[1])+dot(C[2],C[2]))*DomainMinCell.w>wakeSpeed;
    if(!wake)return;
    uint count=uint(round(c.apic0.w));
    uint first;InterlockedAdd(InteriorTotals[5],count,first);
    if(first+count>InteriorTotals[4]){InterlockedAdd(InteriorTotals[8],1);return;}
    for(uint i=0;i<count;++i){uint pid=InteriorFree[first+i];float3 d=interiorNode(c,i);
        FluidParticle p=(FluidParticle)0;p.positionRadius=float4(c.positionRadius.xyz+d,DomainMaxRadius.w);
        p.velocityFlags=float4(c.velocityFlags.xyz+mul(C,d),1);
        p.apic0=float4(C[0],1);p.apic1.xyz=C[1];p.apic2.xyz=C[2];
        Particles[pid]=p;PreviousPositions[pid]=p.positionRadius;
    }
    c=(InteriorCell)0;c.apic2.w=float(InteriorControl.w+30);Interior[id]=c;
    InterlockedAdd(InteriorTotals[3],1);InterlockedAdd(InteriorTotals[10],count);InterlockedAdd(InteriorTotals[12],1);
}
[numthreads(64,1,1)]void InteriorRestore(uint id:SV_DispatchThreadID){restoreInterior(id);}
[numthreads(1,1,1)]void InteriorRestoreOrdered(){
    for(uint id=0;id<InteriorGrid.w;++id)restoreInterior(id);
}
[numthreads(64,1,1)]void InteriorDeposit(uint id:SV_DispatchThreadID){
    if(id>=InteriorGrid.w||InteriorPolicy.z!=0||InteriorPolicy.w!=2||!InteriorControl.y)return;
    InteriorCell old=Interior[id];if(old.velocityFlags.w||old.apic2.w>float(InteriorControl.w)||demandedLod(id)==0)return;
    uint3 base=interiorCoord(id,Grid.xyz)*2;if(any(base<3)||any(base+4>=Grid.xyz)||!safeSolid(id))return;
    float M=0,E=0;float3 dx=0,v=0,lo=1e6,hi=-1e6;uint actual=0;float3 center=coarseCenter(id);float D=.25*DomainMinCell.w*DomainMinCell.w;
    [loop]for(int z=-3;z<=4;++z)[loop]for(int y=-3;y<=4;++y)[loop]for(int x=-3;x<=4;++x){
        uint cell=cellIndex(int3(base)+int3(x,y,z));if(!CellQuanta[cell]||SolidGrid[cell].x<0)return;
    }
    for(uint child=0;child<8;++child){uint cell=cellIndex(base+uint3(child&1,(child>>1)&1,child>>2));
        for(uint j=CellOffsets[cell];j<CellOffsets[cell+1];++j){FluidParticle p=Particles[SortedIndices[j]];float m=p.apic0.w;
            if(m!=1){InterlockedAdd(InteriorTotals[13],1);return;}
            if(length(p.velocityFlags.xyz)+DomainMinCell.w*sqrt(affineSquared(p))>min(InteriorPolicy.x,5e-5))return;
            M+=m;dx+=m*(p.positionRadius.xyz-center);v+=m*p.velocityFlags.xyz;++actual;
            lo=min(lo,p.positionRadius.xyz);hi=max(hi,p.positionRadius.xyz);
            E+=.5*m*(dot(p.velocityFlags.xyz,p.velocityFlags.xyz)+D*affineSquared(p));
        }}
    if(M<32||M>1024||M!=round(M))return;dx/=M;v/=M;
    if(any(abs(dx)>.25*DomainMinCell.w))return;
    // Initial supported low-error policy: only complete resting tensor lattices.
    // Arbitrary deformed/weighted distributions stay fine. This retains exact
    // local density, unlike collapsing an arbitrary cell onto eight point masses.
    float3 spacing=InitialLattice.w?(InitialMaximum.xyz-InitialMinimum.xyz)/float3(InitialLattice.xyz):
        (DomainMaxRadius.xyz-DomainMinCell.xyz)*float3(.5,.42,.5)/max(Counts.z,1u);
    uint3 shape=uint3(round((hi-lo)/spacing))+1;
    InterlockedAdd(InteriorTotals[7],1);
    if(any(shape<2)||any(shape>16)||shape.x*shape.y*shape.z!=actual){InterlockedAdd(InteriorTotals[11],1);return;}
    spacing=(hi-lo)/float3(shape-1); // fit the current settled density, not the pre-projection spawn spacing
    float tolerance=.01*DomainMinCell.w; // at most one percent of a MAC cell; paired density/optical tests gate this policy
    if(any(abs(.5*(hi+lo)-(center+dx))>tolerance)){InterlockedAdd(InteriorTotals[14],1);return;}
    uint used[32];[unroll]for(uint k=0;k<32;++k)used[k]=0;
    float3 moment=D+spacing*spacing*(float3(shape)*float3(shape)-1)/12;
    float3x3 B=0;
    for(uint child=0;child<8;++child){uint cell=cellIndex(base+uint3(child&1,(child>>1)&1,child>>2));
        for(uint j=CellOffsets[cell];j<CellOffsets[cell+1];++j){FluidParticle p=Particles[SortedIndices[j]];
            float3 q=(p.positionRadius.xyz-lo)/spacing;uint3 coordinate=uint3(round(q));
            if(any(coordinate>=shape)||any(abs(q-float3(coordinate))*spacing>tolerance)){InterlockedAdd(InteriorTotals[15],1);return;}
            uint index=(coordinate.z*shape.y+coordinate.y)*shape.x+coordinate.x;
            uint mask=1u<<(index%32);if(used[index/32]&mask)return;used[index/32]|=mask;
            B+=p.apic0.w*(D*float3x3(p.apic0.xyz,p.apic1.xyz,p.apic2.xyz)+outer(p.velocityFlags.xyz-v,p.positionRadius.xyz-center-dx));}}
    float3x3 C=float3x3(B[0]/(M*moment),B[1]/(M*moment),B[2]/(M*moment));
    float after=.5*M*(dot(v,v)+dot(C[0]*C[0]+C[1]*C[1]+C[2]*C[2],moment));
    if(!isfinite(after)||after>E+max(1e-9,2e-6*E)){InterlockedAdd(InteriorTotals[6],1);return;}
    InteriorCell c=(InteriorCell)0;c.positionRadius=float4(center+dx,spacing.x);c.velocityFlags=float4(v,float(shape.x|(shape.y<<5)|(shape.z<<10)));
    c.apic0=float4(C[0],M);c.apic1=float4(C[1],spacing.y);c.apic2=float4(C[2],spacing.z);Interior[id]=c;
    for(uint child=0;child<8;++child){uint cell=cellIndex(base+uint3(child&1,(child>>1)&1,child>>2));
        for(uint j=CellOffsets[cell];j<CellOffsets[cell+1];++j)Particles[SortedIndices[j]]=(FluidParticle)0;}
    InterlockedAdd(InteriorTotals[2],1);InterlockedAdd(InteriorTotals[9],actual);InterlockedAdd(InteriorTotals[12],1);
}
groupshared float4 InteriorReduction[192];
[numthreads(64,1,1)]void InteriorAdvect(uint group:SV_GroupID,uint lane:SV_GroupIndex){
    uint id=InteriorActive[group];InteriorCell c=Interior[id];
    float h=DomainMinCell.w;float3 moment=interiorMoment(c,h),v=0;float3x3 C=0;
    // Transpose of the same separable MAC transfer, including affine moments.
    for(uint a=0;a<3;++a){float3 gp=(c.positionRadius.xyz-DomainMinCell.xyz)/h-faceOffset(a);
        int3 lo=int3(ceil(gp-2.5)),hi=int3(floor(gp+2.5));
        uint3 size=uint3(hi-lo+1);
        for(uint i=lane;i<size.x*size.y*size.z;i+=64){
            int3 node=lo+int3(i%size.x,(i/size.x)%size.y,i/(size.x*size.y));float3 q=float3(node)-gp;
            float w=interiorWeight(c,q,h)/c.apic0.w,u=Faces[faceIndex(node,a)].x;
            v[a]+=w*u;C[a]+=w*u*q*h/moment;
        }InteriorReduction[a*64+lane]=float4(C[a],v[a]);}
    GroupMemoryBarrierWithGroupSync();
    for(uint stride=32;stride;stride/=2){
        if(lane<stride){[unroll]for(uint a=0;a<3;++a)InteriorReduction[a*64+lane]+=InteriorReduction[a*64+lane+stride];}
        GroupMemoryBarrierWithGroupSync();
    }
    if(lane)return;
    [unroll]for(uint a=0;a<3;++a){C[a]=InteriorReduction[a*64].xyz;v[a]=InteriorReduction[a*64].w;}
    c.velocityFlags.xyz=v;c.apic0.xyz=C[0];c.apic1.xyz=C[1];c.apic2.xyz=C[2];
    // Only dormant interiors are admitted. Track displacement and immediately
    // restore moving aggregates in the end-of-frame exchange, not fluid flux
    // across an unrepresented Eulerian/particle boundary.
    c.positionRadius.xyz+=v*GravityDt.w;Interior[id]=c;
}
[numthreads(64,1,1)]void InteriorCount(uint id:SV_DispatchThreadID){
    if(id>=InteriorGrid.w)return;InteriorCell c=Interior[id];if(!c.velocityFlags.w)return;
    uint slot;InterlockedAdd(InteriorTotals[0],1,slot);InteriorActive[slot]=id;
    InterlockedAdd(InteriorTotals[1],uint(round(c.apic0.w*16)));
    uint3 shape=interiorShape(c);
    if(any(shape<2)||shape.x*shape.y*shape.z!=uint(c.apic0.w)||any(interiorSpacing(c)<=0))InterlockedAdd(InteriorTotals[8],1);
    if(any(!isfinite(c.positionRadius))||any(!isfinite(c.velocityFlags))||any(!isfinite(c.apic0))||any(!isfinite(c.apic1))||any(!isfinite(c.apic2)))InterlockedAdd(InteriorTotals[8],1);
}
[numthreads(1,1,1)]void InteriorPrepare(){uint n=(Grid.w+255)/256,p=(Counts.x+255)/256;uint groups[6]={n,p,n,1,n,p};
    [unroll]for(uint i=0;i<6;++i)InteriorArguments.Store3(i*12,uint3(InteriorTotals[12]?groups[i]:0,1,1));
    InteriorArguments.Store3(72,uint3(InteriorTotals[0],1,1));}
