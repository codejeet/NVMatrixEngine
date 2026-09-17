// The FP64 grid ledger owns only retired water, never total (particle + grid)
// liquid. Sample it as bounded volume quadrature in the SAME center field as
// the surface particles. These samples are rendering quadrature, not live
// particles and not an additional simulation inventory.
RWStructuredBuffer<double4> NarrowSurfaceGrid : register(u18);
uint narrowSurfaceIndex(int3 p) {
    uint3 n=(SimulationGrid.xyz+1)/2;
    return (p.z*n.y+p.y)*n.x+p.x;
}
bool narrowSurfaceActive(int3 a,int3 b) {
    for(int z=a.z/2;z<=b.z/2;++z)for(int y=a.y/2;y<=b.y/2;++y)for(int x=a.x/2;x<=b.x/2;++x)
        if(NarrowSurfaceGrid[narrowSurfaceIndex(int3(x,y,z))].w>0)return true;
    return false;
}
void narrowSurfaceGather(float3 p,int3 a,int3 b,inout float wsum,inout float3 offset,
                         inout float3 motion,inout float3x3 metric) {
    const float h=SimulationMinimumCell.w, support=Reconstruction.x;
    for(int z=a.z/2;z<=b.z/2;++z)for(int y=a.y/2;y<=b.y/2;++y)for(int x=a.x/2;x<=b.x/2;++x) {
        double4 owned=NarrowSurfaceGrid[narrowSurfaceIndex(int3(x,y,z))];
        if(owned.w<=0)continue;
        // Full owners tile the interior; a partial/deferred owner occupies a
        // volume-scaled quadrature cube. A tiny transported tail must NOT turn
        // a whole coarse cell into negative phi through weight normalization.
        float fraction=saturate(float(owned.w/(8.0*(double)h*(double)h*(double)h)));
        float scale=max(1e-6,pow(fraction,1.0/3.0));
        float mass=float(owned.w/(64.0*(double)PhaseControl.w));
        float3 center=SimulationMinimumCell.xyz+(float3(x,y,z)*2+1)*h;
        float3 displacement=-float3(owned.xyz/owned.w)*PhaseControl.x;
        float inv=1/scale;
        for(uint i=0;i<64;++i) {
            float3 local=(float3(i&3,(i>>2)&3,i>>4)*.5-.75)*h;
            float3 q=center+local*scale-p, gq=q*inv;
            float w=max(0,1-dot(gq,gq)/(support*support));
            if(w==0)continue;
            w=w*w*w*inv*inv*inv*mass;
            wsum+=w;offset+=q*w;motion+=displacement*w;
            metric+=float3x3(inv,0,0,0,inv,0,0,0,inv)*w;
        }
    }
}
