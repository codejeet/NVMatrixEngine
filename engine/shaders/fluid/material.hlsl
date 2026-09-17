#include "common.hlsli"
#ifndef BULK_PRESSURE
#define BULK_PRESSURE 0
#endif
#if BULK_PRESSURE
RWStructuredBuffer<float> BulkFilledVolumeFraction : register(u36);
#endif
// Constant-coefficient Laplacian viscosity, split before pressure. Explicit
// subcycles enforce 6*alpha <= .9 (a convex combination, no unstable diffusion).
// Invalid/free faces use zero normal derivative; this V1 is NOT the full
// coupled viscous stress/free-surface traction solve of Batty & Bridson 2008.
[numthreads(128,1,1)]
void Viscosity(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x,stride=faceStride();if(id>=stride*3)return;
    uint axis=id/stride,k=id%stride;
    int3 p=int3(k%(Grid.x+1),(k/(Grid.x+1))%(Grid.y+1),k/((Grid.x+1)*(Grid.y+1)));
    int3 extent=int3(Grid.xyz);extent[axis]++;
    float4 f=Faces[id];
    if(any(p>=extent)||f.z<=1e-8){FaceScratch[id]=f;return;}
    int3 left=p;left[axis]--;
#if CUT_PRESSURE
    if(cutPressureAperture(p,axis)==0){f.x=cutPressureBoundary(p,axis);FaceScratch[id]=f;return;}
#else
    if(solid(left)||solid(p)){f.x=boundaryVelocity(left,p,axis);FaceScratch[id]=f;return;}
#endif
    float lap=0;
    [unroll]for(uint a=0;a<3;++a)[unroll]for(int side=-1;side<=1;side+=2) {
        int3 n=p;n[a]+=side;if(any(n<0)||any(n>=extent))continue;
        int3 l=n;l[axis]--;
        float4 neighbor=Faces[faceIndex(n,axis)];
#if CUT_PRESSURE
        if(cutPressureAperture(n,axis)==0)lap+=cutPressureBoundary(n,axis)-f.x;
#else
        if(solid(l)||solid(n))lap+=boundaryVelocity(l,n,axis)-f.x;
#endif
        else if(neighbor.z>1e-8)lap+=neighbor.x-f.x;
    }
    f.x+=Material.x*lap;
    FaceScratch[id]=f; // Preserve .y exactly: FLIP must see the viscous delta.
}
float occupancy(int3 c) {
    if(!inGrid(c))return 0;
    float value=saturate(cellMass(cellIndex(c))*Solver.w);
#if BULK_PRESSURE
    // Union of particle coverage and filled Eulerian volume, never a sum of
    // two representations of the same water. Reuse the same spatial smoothing.
    value=max(value,BulkFilledVolumeFraction[cellIndex(c)]);
#endif
    return value;
}
[numthreads(256,1,1)]
void SurfaceColor(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x;if(id>=Grid.w)return;
    int3 c=int3(cellFromIndex(id));float colour=0;
    // Smooth particle-volume occupancy (not the rendered SDF). No particle
    // neighbors here: 27 compact grid loads. Curvature is simulation-scale.
    [unroll]for(int z=-1;z<=1;++z)[unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x)
        colour+=occupancy(c+int3(x,y,z))*(x==0?2.0/3:1.0/6)*(y==0?2.0/3:1.0/6)*(z==0?2.0/3:1.0/6);
    if(Material.w==2) {
        float3 p=DomainMinCell.xyz+(float3(c)+.5)*DomainMinCell.w;
        colour=saturate(.5-(length(p-(DomainMinCell.xyz+DomainMaxRadius.xyz)*.5)-.75)/(6*DomainMinCell.w));
    }
    Density[id]=float4(colour,0,0,0);
}
float colourAt(int3 c) {return Density[cellIndex(clamp(c,0,int3(Grid.xyz)-1))].x;}
[numthreads(256,1,1)]
void SurfaceCurvature(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x;if(id>=Grid.w)return;
    int3 p=int3(cellFromIndex(id));float h=DomainMinCell.w,c=colourAt(p);
    float3 g=0;float3x3 H=(float3x3)0;
    bool contact=false;
    [unroll]for(uint a=0;a<3;++a) {
        int3 e=0;e[a]=1;
        float lo=colourAt(p-e),hi=colourAt(p+e);
        g[a]=(hi-lo)/(2*h);H[a][a]=(hi-2*c+lo)/(h*h);
        contact=contact||solid(p-e)||solid(p+e)||solid(p-2*e)||solid(p+2*e);
        [unroll]for(uint b=0;b<3;++b)if(b>a) {
            int3 f=0;f[b]=1;
            float v=(colourAt(p+e+f)-colourAt(p+e-f)-colourAt(p-e+f)+colourAt(p-e-f))/(4*h*h);
            H[a][b]=v;H[b][a]=v;
        }
    }
    float gg=dot(g,g),kappa=0;
    if(gg>1e-6&&!contact)
        kappa=-(gg*(H[0][0]+H[1][1]+H[2][2])-dot(g,mul(H,g)))/pow(gg,1.5);
    // Under-resolved curvature is bounded, not advertised as tiny-droplet physics.
    // Suppress contact-line tension until contact angles/wetting are implemented.
    MaterialGrid[id]=float4(c,clamp(kappa,-2/h,2/h),sqrt(gg),0);
}
[numthreads(128,1,1)]
void MaterialFixture(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x,stride=faceStride();if(id>=stride*3)return;
    uint axis=id/stride,k=id%stride;
    uint3 p=uint3(k%(Grid.x+1),(k/(Grid.x+1))%(Grid.y+1),k/((Grid.x+1)*(Grid.y+1)));
    uint3 extent=Grid.xyz;extent[axis]++;
    float v=axis==0?sin((float(p.z)+.5)*.4):0;
    Faces[id]=any(p>=extent)?0:float4(v,v,1,0);
}
