#include "common.hlsli"
// V1 free surface: zero air pressure, impermeable domain walls. Solid SDF
// fractions/boundary velocities will replace the domain-only boundary in M6.
[numthreads(256,1,1)]
void Classify(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x;if(id>=Grid.w)return;
    Cells[id]=float4(0,0,solid(cellFromIndex(id))?2:(cellOccupied(id)?1:0),0);PressureIn[id]=0;PressureOut[id]=0;Density[id]=0;MaterialGrid[id]=0;
}
[numthreads(128,1,1)]
void Forces(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x,stride=faceStride();if(id>=stride*3)return;
    uint axis=id/stride,k=id%stride;
    uint3 p=uint3(k%(Grid.x+1),(k/(Grid.x+1))%(Grid.y+1),k/((Grid.x+1)*(Grid.y+1)));
    uint3 extent=Grid.xyz;extent[axis]++;
    if(any(p>=extent))return;
    float4 f=Faces[id];
    int3 left=int3(p);left[axis]--;
#if CUT_PRESSURE
    if(cutPressureAperture(p,axis)==0)f.x=cutPressureBoundary(p,axis);
#else
    if(solid(left)||solid(int3(p)))f.x=boundaryVelocity(left,int3(p),axis);
#endif
    else if(f.z>1e-8&&Counts.w!=3)f.x+=GravityDt[axis]*GravityDt.w;
    Faces[id]=f;
}
float cellDivergence(uint3 p) {
    float d=0;
#if CUT_PRESSURE
    [unroll]for(uint a=0;a<3;++a){uint3 next=p;next[a]++;d+=cutPressureAperture(next,a)*Faces[faceIndex(next,a)].x-cutPressureAperture(p,a)*Faces[faceIndex(p,a)].x;}
    return (d+cutPressureSource(p))/(DomainMinCell.w*cutPressureVolume(p));
#else
    [unroll]for(uint a=0;a<3;++a){uint3 next=p;next[a]++;d+=Faces[faceIndex(next,a)].x-Faces[faceIndex(p,a)].x;}
    return d/DomainMinCell.w;
#endif
}
[numthreads(256,1,1)]
void Divergence(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x;if(id>=Grid.w)return;
    if(Cells[id].z!=1){Cells[id].x=0;FaceScratch[id]=0;return;}
    int3 c=int3(cellFromIndex(id));float divergence=cellDivergence(c);
    Cells[id].x=divergence;
    // Geometry and capillary ghost pressures are invariant over all Jacobi
    // iterations. Assemble once, retaining the same six-point Poisson operator.
    uint mask=0,diagonal=0;float ghost=0;
    [unroll]for(uint axis=0;axis<3;++axis)[unroll]for(int side=-1;side<=1;side+=2) {
        int3 n=c;n[axis]+=side;
        if(!solid(n)) {
            diagonal++;
            if(liquid(n))mask|=1u<<(axis*2+(side>0?1:0));
            else if(Material.y>0)ghost+=capillaryPressure(c,n);
        }
    }
    float rhs=Solver.x*DomainMinCell.w*DomainMinCell.w/GravityDt.w*divergence;
    FaceScratch[id]=float4(ghost-rhs,diagonal>0?1.0/diagonal:0,float(mask),0);
}
[numthreads(256,1,1)]
void Jacobi(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x;if(id>=Grid.w)return;
    PressureOut[id]=pressureStencil(id);
}
[numthreads(128,1,1)]
void Project(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x,stride=faceStride();if(id>=stride*3)return;
    uint axis=id/stride,k=id%stride;
    int3 right=int3(k%(Grid.x+1),(k/(Grid.x+1))%(Grid.y+1),k/((Grid.x+1)*(Grid.y+1)));
    uint3 extent=Grid.xyz;extent[axis]++;if(any(right>=int3(extent)))return;
    int3 left=right;left[axis]--;
    float4 f=Faces[id];
    if(solid(left)||solid(right)){f.x=boundaryVelocity(left,right,axis);f.w=1;}
    else if(liquid(left)||liquid(right)) {
        float pl=PressureIn[cellIndex(left)],pr=PressureIn[cellIndex(right)];
        if(Material.y>0) {
            if(!liquid(left))pl=capillaryPressure(right,left);
            if(!liquid(right))pr=capillaryPressure(left,right);
        }
        f.x-=GravityDt.w/(Solver.x*DomainMinCell.w)*(pr-pl);
        f.w=1;
    }else f.w=0;
    Faces[id]=f;
}
[numthreads(256,1,1)]
void Measure(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x;if(id>=Grid.w)return;
    Cells[id].y=Cells[id].z==1?cellDivergence(cellFromIndex(id)):0;
    Cells[id].w=PressureIn[id];
}
