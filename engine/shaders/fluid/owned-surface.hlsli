// Optional joint-Solver geometry. Total phase already includes particle owners:
// adding particle kernels here would count their volume a second time.
#include "owned-phase.hlsli"
RWStructuredBuffer<double2> OwnedSurfacePhase : register(u18);
RWStructuredBuffer<double4> OwnedSurfacePlane : register(u19);
RWStructuredBuffer<float4> OwnedSurfaceFaces : register(u20);
// Two field intervals leave an unsaturated band around the crossing and the
// filtered-normal stencil. A half-interval box followed by linear occupancy
// remapping displaced even axis-aligned, nonsnapped planes.
static const float OwnedSurfaceFilterRadius=1.0; // fine MAC cells
uint ownedSurfaceIndex(uint3 p){uint3 g=(SimulationGrid.xyz+1)/2;return (p.z*g.y+p.y)*g.x+p.x;}
#include "owned-height.hlsli"
bool ownedSurfaceActive(uint brick) {
    // Field nodes use half-MAC-cell spacing, with two MAC cells of padding.
    float3 lo=float3(brickCoord(brick)*4)-2;
    int3 a=max(0,int3(floor((lo-OwnedSurfaceFilterRadius)*.5)));
    int3 b=min(int3((SimulationGrid.xyz+1)/2)-1,int3(floor((lo+4+OwnedSurfaceFilterRadius)*.5)));
    for(int z=a.z;z<=b.z;++z)for(int y=a.y;y<=b.y;++y)for(int x=a.x;x<=b.x;++x)
        if(OwnedSurfacePhase[ownedSurfaceIndex(uint3(x,y,z))].x>0.0)return true;
    return false;
}
float3 ownedSurfaceVelocity(float3 position) {
    float3 velocity=0;uint3 padded=SimulationGrid.xyz+1;
    [unroll]for(uint axis=0;axis<3;++axis){
        float3 offset=.5;offset[axis]=0;
        float3 q=position-offset;int3 base=int3(floor(q));float3 f=q-float3(base);
        int3 last=int3(SimulationGrid.xyz)-1;last[axis]++;
        [unroll]for(uint k=0;k<8;++k){
            uint3 e=uint3(k&1,(k>>1)&1,k>>2),p=uint3(clamp(base+int3(e),0,last));
            float3 weight=lerp(1-f,f,float3(e));
            uint face=axis*padded.x*padded.y*padded.z+(p.z*padded.y+p.y)*padded.x+p.x;
            velocity[axis]+=OwnedSurfaceFaces[face].x*weight.x*weight.y*weight.z;
        }
    }
    return velocity;
}
float4 reconstructOwnedNode(uint3 node) {
    // Integrate liquid volume over a symmetric box, not min(cell SDF). The
    // latter creates phantom interior walls at shared fully wet cell faces.
    // Integer-derived coordinates keep duplicated brick-face nodes identical.
    float3 p=float3(node)*.5-2;
    float3 filterLo=max(p-OwnedSurfaceFilterRadius,0);
    float3 filterHi=min(p+OwnedSurfaceFilterRadius,float3(SimulationGrid.xyz));
    int3 a=max(0,int3(floor(filterLo*.5)));
    int3 b=min(int3((SimulationGrid.xyz+1)/2)-1,int3(floor(filterHi*.5)));
    double liquid=0.0;
    double3 normalSum=double3(0,0,0);
    for(int z=a.z;z<=b.z;++z)for(int y=a.y;y<=b.y;++y)for(int x=a.x;x<=b.x;++x){
        uint3 owner=uint3(x,y,z);uint id=ownedSurfaceIndex(owner);
        double2 phase=OwnedSurfacePhase[id];if(phase.x==0.0)continue;
        float3 lower=float3(owner*2),width=float3(min(uint3(2,2,2),SimulationGrid.xyz-2*owner));
        double3 lo=double3(max(filterLo,lower)-lower)/double3(width);
        double3 hi=double3(min(filterHi,lower+width)-lower)/double3(width);
        double3 extent=(hi-lo)*double3(width);
        if(any(extent<=double3(0,0,0)))continue;
        double4 plane=OwnedSurfacePlane[id];
        double fraction=ownedPhaseBoxFraction(phase,plane,lo,hi);
        double volume=extent.x*extent.y*extent.z;
        liquid+=volume*fraction;
        // Convert the reflected-corner PLIC normal into the common physical
        // metric before averaging. Odd-width boundary owners must not tilt it.
        // Only cut portions carry interface orientation; full/empty owners do
        // not introduce fictitious normals at their shared faces.
        if(fraction>0.0 && fraction<1.0){
            double3 n=plane.xyz/double3(width);
            double norm2=n.x*n.x+n.y*n.y+n.z*n.z;
            if(norm2>0.0){
                double weight=volume*fraction*(1.0-fraction)/double(sqrt(float(norm2)));
                normalSum+=n*weight;
            }
        }
    }
    // This is a continuous scalar, NOT a distance bound. Existing trilinear DDA
    // and polynomial root refinement remain the canonical intersection method.
    // The bounded simulation domain is known exactly. Normalize the truncated
    // filter at its walls; the consumer intersects with the analytic boundary.
    // Composing the boundary at NODES bends the interpolated free surface and
    // its normals near walls. Treating absent out-of-domain samples as air
    // rounds away pool edges/corners and
    // loses a substantial amount of rendered volume even for a planar box.
    float3 extent=max(0,filterHi-filterLo);
    double filterVolume=double(extent.x)*double(extent.y)*double(extent.z);
    double fraction=filterVolume>0.0?liquid/filterVolume:0.0;
    float occupancy=saturate(float(fraction));
    float signedPosition=2*OwnedSurfaceFilterRadius*(.5-occupancy);
    double3 absolute=double3(normalSum.x<0.0?-normalSum.x:normalSum.x,
                            normalSum.y<0.0?-normalSum.y:normalSum.y,normalSum.z<0.0?-normalSum.z:normalSum.z);
    double normalScale=absolute.x>absolute.y?absolute.x:absolute.y;
    normalScale=normalScale>absolute.z?normalScale:absolute.z;
    if(fraction>0.0 && fraction<1.0 && normalScale>0.0){
        float heightPosition;
        if(ownedHeightPosition(p,normalSum,heightPosition)){
            Counters.InterlockedAdd(20,1);
            float3 motion=PhaseControl.x>0?-ownedSurfaceVelocity(p)*PhaseControl.x:0;
            return float4(SimulationMinimumCell.w*heightPosition,motion);
        }
        Counters.InterlockedAdd(24,1);
        // Invert the box integral instead of treating occupancy as distance.
        // This is a local volume-matched plane fit, exact for a planar PLIC
        // neighborhood at arbitrary orientation/offset, including clipped
        // domain filters. Curved/multisheet neighborhoods remain approximations
        // requiring surface-error/refinement qualification, not SDF bounds.
        double3 n=normalSum/normalScale;
        n/=double(sqrt(float(n.x*n.x+n.y*n.y+n.z*n.z)));
        double3 positive=double3(n.x<0.0?-n.x:n.x,n.y<0.0?-n.y:n.y,n.z<0.0?-n.z:n.z);
        double3 support=positive*double3(extent);
        double span=support.x+support.y+support.z,alpha;
        if(fraction==.5)alpha=span*.5;
        else if((support.x>0.0?1:0)+(support.y>0.0?1:0)+(support.z>0.0?1:0)==1)alpha=span*fraction;
        else {
            double low=0.0,high=span;
            [loop]for(uint iteration=0;iteration<32;++iteration){
                double mid=(low+high)*.5;
                if(ownedPlaneFraction(support,mid)<fraction)low=mid;else high=mid;
            }
            alpha=(low+high)*.5;
        }
        double3 relative=double3(p-(filterLo+filterHi)*.5);
        signedPosition=float(n.x*relative.x+n.y*relative.y+n.z*relative.z+
                             span*.5-alpha);
    }
    float phi=SimulationMinimumCell.w*signedPosition;
    // Sub-lattice features still require refinement/hybrid reconstruction;
    // neither this filter nor the contour changes any simulation inventory.
    float3 motion=occupancy>0&&PhaseControl.x>0?-ownedSurfaceVelocity(p)*PhaseControl.x:0;
    return float4(phi,motion);
}
