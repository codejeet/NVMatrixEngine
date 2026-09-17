// CUDA joint-Solver output ABI. These are non-owning geometry descriptors, not
// a second volume inventory, not a nodal SDF, and not directly ray-traceable.
// Continuous reconstruction integrates neighboring cells into the canonical
// Field; occupancy below is for descriptor validation, never direct ray marching.
// Plane normal is signed L1-normalized. Alpha is measured from the liquid-side
// reflected corner, preserving thin regions near either end of a cell.
float ownedPhaseOccupancy(double2 phase, double4 plane, float3 unitPosition) {
    if (phase.x == 0.0) return 0;
    if (phase.x == phase.y) return 1;
    if (all(plane.xyz == double3(0,0,0))) return float(phase.x / phase.y);
    double3 q = double3(unitPosition);
    q.x = plane.x < 0.0 ? 1.0-q.x : q.x;
    q.y = plane.y < 0.0 ? 1.0-q.y : q.y;
    q.z = plane.z < 0.0 ? 1.0-q.z : q.z;
    // HLSL's vector intrinsics may narrow double operands to float.
    double x = plane.x < 0.0 ? -plane.x : plane.x;
    double y = plane.y < 0.0 ? -plane.y : plane.y;
    double z = plane.z < 0.0 ? -plane.z : plane.z;
    return x*q.x+y*q.y+z*q.z < plane.w ? 1 : 0;
}
// Analytic half-space integral, matching the signed/reflected export convention.
// Scalar FP64 arithmetic is deliberate: HLSL vector intrinsics can narrow to FP32.
double ownedPlaneFraction(double3 n, double alpha) {
    double a=n.x, b=n.y, c=n.z;
    if(a>b){double t=a;a=b;b=t;}
    if(b>c){double t=b;b=c;c=t;}
    if(a>b){double t=a;a=b;b=t;}
    double sum=a+b+c;
    if(alpha<=0.0)return 0.0;
    if(alpha>=sum)return 1.0;
    bool complement=alpha>sum*.5;
    if(complement)alpha=sum-alpha;
    double value;
    if(b==0.0)value=alpha/c;
    else if(a==0.0)value=alpha<b?alpha*alpha/(2.0*b*c):(alpha-b*.5)/c;
    else if(alpha<a)value=(alpha/a)*(alpha/b)*(alpha/c)/6.0;
    else if(alpha>=a+b)value=(alpha-(a+b)*.5)/c;
    else {
        double v=3.0*alpha*(alpha-a)+a*a;
        if(alpha>b){double d=alpha-b;v-=(d/a)*d*d;}
        if(alpha>c){double d=alpha-c;v-=(d/a)*d*d;}
        value=v/(6.0*b*c);
    }
    if(complement)value=1.0-value;
    return value<0.0?0.0:(value>1.0?1.0:value);
}
double ownedPhaseBoxFraction(double2 phase, double4 plane, double3 lo, double3 hi) {
    if(phase.x==0.0)return 0.0;
    if(phase.x==phase.y)return 1.0;
    if(all(plane.xyz==double3(0,0,0)))return phase.x/phase.y;
    double3 n=double3(plane.x<0.0?-plane.x:plane.x,
                     plane.y<0.0?-plane.y:plane.y,plane.z<0.0?-plane.z:plane.z);
    double alpha=plane.w-n.x*(plane.x<0.0?1.0-hi.x:lo.x)
                        -n.y*(plane.y<0.0?1.0-hi.y:lo.y)
                        -n.z*(plane.z<0.0?1.0-hi.z:lo.z);
    return ownedPlaneFraction(n*(hi-lo),alpha);
}
