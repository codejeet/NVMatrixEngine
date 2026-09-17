#ifndef FLUID_DENSITY_KERNEL
#define FLUID_DENSITY_KERNEL
struct FluidSolidKernel {float mass;uint2 fingerprint;uint rebuilt;};
uint2 densityKernelHash(uint2 hash,uint value){
    hash.x=(hash.x^value)*16777619u;
    hash.y^=value+0x9e3779b9u+(hash.y<<6)+(hash.y>>2);
    return hash;
}
// Integral of the normalized quadratic B-spline, in grid units. The reflected
// form avoids cancellation in the small tails next to solid boundaries.
float densityKernelCDF(float x){
    float a=abs(x),t=1.5-a;
    // DXC lowers pow(t,3) to Log/Exp even for this integer exponent. Keep the
    // exact polynomial explicit in the innermost surface-integration loop.
    float tail=a>=1.5?0:(a>=.5?t*t*t/6:.5-.75*a+a*a*a/3);
    return x<0?tail:1-tail;
}
float densityKernelIntegral(float lo,float hi){
    return lo>=0?densityKernelCDF(-lo)-densityKernelCDF(-hi):densityKernelCDF(hi)-densityKernelCDF(lo);
}
float densityKernelValue(float x){
    x=abs(x);return x<.5?.75-x*x:(x<1.5?.5*(1.5-x)*(1.5-x):0);
}
// The same 000--111 Freudenthal tetrahedra used by cut-cell capacities.
float densityTetPhi(float3 p,float phi[8]){
    uint a=0,b=1,c=2;
    if(p[a]<p[b]){uint t=a;a=b;b=t;}
    if(p[b]<p[c]){uint t=b;b=c;c=t;}
    if(p[a]<p[b]){uint t=a;a=b;b=t;}
    return phi[0]+p[a]*(phi[1u<<a]-phi[0])+p[b]*(phi[(1u<<a)|(1u<<b)]-phi[1u<<a])+
        p[c]*(phi[7]-phi[(1u<<a)|(1u<<b)]);
}
float densitySolidSegment(float lo,float hi,float pLo,float pHi,float offset,float width){
    if(pLo>=0&&pHi>=0)return 0;
    if(pLo>=0||pHi>=0){float root=lerp(lo,hi,saturate(pLo/(pLo-pHi)));
        if(pLo>=0)lo=root;else hi=root;}
    return densityKernelIntegral(offset+lo*width,offset+hi*width);
}
// Integrate exactly along the dominant solid-normal axis, splitting at every
// tetrahedron crossing. Six-point Gaussian quadrature covers the other axes.
// Axis-aligned planes (including fractional terminal cells) are exact; curved
// and oblique sampled geometry uses bounded, independently audited quadrature.
float densitySolidKernel(float3 offset,float3 width,float phi[8]){
    float3 gradient=float3(phi[1]+phi[3]+phi[5]+phi[7]-phi[0]-phi[2]-phi[4]-phi[6],
        phi[2]+phi[3]+phi[6]+phi[7]-phi[0]-phi[1]-phi[4]-phi[5],
        phi[4]+phi[5]+phi[6]+phi[7]-phi[0]-phi[1]-phi[2]-phi[3])/width;
    gradient=abs(gradient);uint a=gradient.y>gradient.x?1:0;a=gradient.z>gradient[a]?2:a;
    uint b=(a+1)%3,c=(a+2)%3;
    // This is an exact specialization of the sampled field, not a flatness
    // tolerance or a scene-specific floor shortcut. Most basin walls qualify.
    bool axisPlane=true;
    [unroll]for(uint corner=0;corner<8;++corner)axisPlane=axisPlane&&(phi[corner]==phi[corner&(1u<<a)]);
    if(axisPlane)return densitySolidSegment(0,1,phi[0],phi[1u<<a],offset[a],width[a])*
        densityKernelIntegral(offset[b],offset[b]+width[b])*densityKernelIntegral(offset[c],offset[c]+width[c]);
    const float nodes[6]={.0337652429,.1693953068,.380690407,.619309593,.8306046932,.9662347571};
    const float weights[6]={.0856622462,.1803807865,.2339569673,.2339569673,.1803807865,.0856622462};
    float sum=0;
    [loop]for(uint i=0;i<6;++i)[loop]for(uint j=0;j<6;++j){
        float3 p=0;p[b]=nodes[i];p[c]=nodes[j];float points[4]={0,min(p[b],p[c]),max(p[b],p[c]),1};
        p[a]=0;float previous=densityTetPhi(p,phi),integral=0;
        [unroll]for(uint k=0;k<3;++k){p[a]=points[k+1];float next=densityTetPhi(p,phi);
            integral+=densitySolidSegment(points[k],points[k+1],previous,next,offset[a],width[a]);previous=next;}
        sum+=integral*weights[i]*weights[j]*densityKernelValue(offset[b]+nodes[i]*width[b])*
            densityKernelValue(offset[c]+nodes[j]*width[c])*width[b]*width[c];
    }
    return sum;
}
#endif
