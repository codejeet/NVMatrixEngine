// Shared with the CPU optical estimator test; keep both modes on the same root predicate.
float liquidPolynomial(float4 c,float t){return ((c.w*t+c.z)*t+c.y)*t+c.x;}
#ifdef __cplusplus
bool liquidRoot(float4 c,float& root,bool visibilityOnly=false) {
#else
bool liquidRoot(float4 c,out float root,bool visibilityOnly=false) {
#endif
    // Most crossing cells already have opposite endpoint signs. The IVT proves
    // visibility without even solving the derivative; equal-sign endpoints
    // still take the extrema path below so thin sheets cannot disappear.
    if(visibilityOnly) {
        float end=liquidPolynomial(c,1);
        if((c.x<0)!=(end<0)){root=.5;return true;}
    }
    float cuts[4];cuts[0]=0;uint count=1;
    float a=3*c.w,b=2*c.z,d=c.y;
    if(abs(a)<1e-12) {
        if(abs(b)>1e-12){float r=-d/b;if(r>0&&r<1)cuts[count++]=r;}
    }else {
        float disc=b*b-4*a*d;
        if(disc>=0){float s=sqrt(disc),r0=(-b-s)/(2*a),r1=(-b+s)/(2*a);
            if(r0>r1){float tmp=r0;r0=r1;r1=tmp;}
            if(r0>0&&r0<1)cuts[count++]=r0;
            if(r1>0&&r1<1&&r1>r0+1e-7)cuts[count++]=r1;}
    }
    cuts[count++]=1;root=0;
    for(uint i=0;i+1<count;++i) {
        float lo=cuts[i],hi=cuts[i+1],fl=liquidPolynomial(c,lo),fh=liquidPolynomial(c,hi);
        if(abs(fl)<1e-8){root=lo;return true;}
        if((fl<0)!=(fh<0)) {
            // An opaque, first-hit visibility ray only needs proof that a root
            // exists inside its clipped interval. This monotone sign bracket is
            // that proof; its exact distance is unused. Camera/photon rays still
            // refine all 16 steps, including thin sheets and internal crossings.
            if(visibilityOnly){root=.5*(lo+hi);return true;}
            // Sixteen refinements also bound world-space error at the large
            // room's 32cm render cells; twelve could exceed the 50um residual
            // gate even for correctly bracketed roots. Only hit cells pay this.
            #ifndef __cplusplus
            [unroll]
            #endif
            for(uint j=0;j<16;++j){float m=.5*(lo+hi),fm=liquidPolynomial(c,m);if((fm<0)==(fl<0)){lo=m;fl=fm;}else hi=m;}
            root=.5*(lo+hi);return true;
        }
        if(abs(fh)<1e-8){root=hi;return true;}
    }
    return false;
}
