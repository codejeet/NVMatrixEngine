// Covariance shaping inspired by Yu/Turk 2010, applied to a normalized
// weighted-center field. This is not their SPH density-sum reconstruction.
struct ParticleShape {float4 row0,row1,row2;}; // inverse axes; w = center displacement
RWStructuredBuffer<ParticleShape> Shapes : register(u10);
float3x3 shapeMatrix(ParticleShape p){return float3x3(p.row0.xyz,p.row1.xyz,p.row2.xyz);}
float3 shapeOffset(ParticleShape p){return float3(p.row0.w,p.row1.w,p.row2.w);}
float3x3 outerProduct(float3 a){return float3x3(a.x*a,a.y*a,a.z*a);}
void diagonalize(inout float3x3 a,out float3x3 v) {
    v=float3x3(1,0,0,0,1,0,0,0,1);
    // Fixed small symmetric Jacobi solve: eigenvectors remain orthonormal even
    // for repeated eigenvalues; clamping handles collinear/singular neighborhoods.
    [unroll]for(uint sweep=0;sweep<5;++sweep)[unroll]for(uint pair=0;pair<3;++pair) {
        uint p=pair==2?1:0,q=pair==0?1:2;
        if(abs(a[p][q])<1e-10)continue;
        float theta=(a[q][q]-a[p][p])/(2*a[p][q]);
        float t=(theta>=0?1:-1)/(abs(theta)+sqrt(1+theta*theta));
        float c=rsqrt(1+t*t),s=t*c;
        float3x3 j=float3x3(1,0,0,0,1,0,0,0,1);
        j[p][p]=j[q][q]=c;j[p][q]=s;j[q][p]=-s;
        a=mul(transpose(j),mul(a,j));v=mul(v,j);
    }
}
[numthreads(128,1,1)]
void SurfaceShape(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x;if(id>=uint(Reconstruction.z))return;
    ParticleShape shape;shape.row0=float4(1,0,0,0);shape.row1=float4(0,1,0,0);shape.row2=float4(0,0,1,0);
    if(Particles[id].velocityFlags.w==0||Collision.y==0){Shapes[id]=shape;return;}
    float3 p=Particles[id].positionRadius.xyz;float radius=SimulationMinimumCell.w*2;
    int3 a=max(0,int3(floor((p-radius-SimulationMinimumCell.xyz)/SimulationMinimumCell.w)));
    int3 b=min(int3(SimulationGrid.xyz)-1,int3(floor((p+radius-SimulationMinimumCell.xyz)/SimulationMinimumCell.w)));
    float sum=0;float3 mean=0;float3x3 covariance=0;float neighbors=0;
    for(int z=a.z;z<=b.z;++z)for(int y=a.y;y<=b.y;++y)for(int x=a.x;x<=b.x;++x) {
        uint cell=simIndex(int3(x,y,z));
        for(uint j=Offsets[cell];j<Offsets[cell+1];++j) {
            float3 q=Particles[Indices[j]].positionRadius.xyz-p;
            float w=max(0,1-dot(q,q)/(radius*radius));w=w*w*w;
            if(w==0)continue; // Compact support: no mass/covariance work outside it.
            float weight=Particles[Indices[j]].apic0.w;
            if(w>.01)neighbors+=weight;w*=weight;
            sum+=w;mean+=q*w;covariance+=outerProduct(q)*w;
        }
    }
    if(neighbors>=8&&sum>1e-8) {
        mean/=sum;covariance=covariance/sum-outerProduct(mean);
        float3x3 v;diagonalize(covariance,v);
        float3 eigen=max(float3(covariance[0][0],covariance[1][1],covariance[2][2]),1e-8);
        float largest=max(eigen.x,max(eigen.y,eigen.z));
        // Longest axis retains the isotropic support; the two shorter axes
        // contract. Relative elongation captures sheets/tendrils without a large
        // expanded gather halo. Kernel determinant normalization is applied below.
        float3 axes=sqrt(max(eigen,largest*.25)/largest);
        axes=lerp(1,axes,.65*smoothstep(8,24,float(neighbors)));
        float3 inv=1/axes;
        float3x3 g=mul(v,mul(float3x3(inv.x,0,0,0,inv.y,0,0,0,inv.z),transpose(v)));
        float3 center=mean*.1;
        shape.row0=float4(g[0],center.x);shape.row1=float4(g[1],center.y);shape.row2=float4(g[2],center.z);
    }
    if(any(!isfinite(shape.row0))||any(!isfinite(shape.row1))||any(!isfinite(shape.row2)))Counters.InterlockedAdd(8,1);
    Shapes[id]=shape;
}
