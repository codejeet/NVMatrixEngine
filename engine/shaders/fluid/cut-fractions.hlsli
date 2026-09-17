#ifndef FLUID_CUT_FRACTIONS
#define FLUID_CUT_FRACTIONS
// Measure of phi >= 0 for a linearly interpolated simplex. These are geometric
// fractions, not smoothed Heaviside samples. Equal/zero distances never divide
// by zero: interpolation is only across edges with opposite classifications.
float cutEdge(float inside,float outside){return inside/(inside-outside);}
float cutTriangle(float3 phi){
    float positive[3],negative[3];uint ni=0,no=0;
    [unroll]for(uint i=0;i<3;++i){if(phi[i]>=0)positive[ni++]=phi[i];else negative[no++]=phi[i];}
    if(ni==0)return 0;if(ni==3)return 1;
    if(ni==1)return cutEdge(positive[0],negative[0])*cutEdge(positive[0],negative[1]);
    return 1-cutEdge(-negative[0],-positive[0])*cutEdge(-negative[0],-positive[1]);
}
float cutTetrahedron(float4 phi){
    float positive[4],negative[4];uint ni=0,no=0;
    [unroll]for(uint i=0;i<4;++i){if(phi[i]>=0)positive[ni++]=phi[i];else negative[no++]=phi[i];}
    if(ni==0)return 0;if(ni==4)return 1;
    if(ni==1)return cutEdge(positive[0],negative[0])*cutEdge(positive[0],negative[1])*cutEdge(positive[0],negative[2]);
    if(ni==3)return 1-cutEdge(-negative[0],-positive[0])*cutEdge(-negative[0],-positive[1])*cutEdge(-negative[0],-positive[2]);
    // The two-inside clipped wedge is three non-overlapping tetrahedra.
    float a=cutEdge(positive[0],negative[0]),b=cutEdge(positive[0],negative[1]);
    float c=cutEdge(positive[1],negative[0]),d=cutEdge(positive[1],negative[1]);
    return a*b+a*d*(1-b)+c*d*(1-a);
}
float cutCube(float phi[8]){
    // Freudenthal subdivision along 000--111. Every shared axis face has the
    // same low--high diagonal, including coarse/fine restriction boundaries.
    static const uint3 faces[6]={uint3(1,3,7),uint3(1,5,7),uint3(2,3,7),
        uint3(2,6,7),uint3(4,5,7),uint3(4,6,7)};
    float volume=0;
    [unroll]for(uint i=0;i<6;++i)volume+=cutTetrahedron(float4(phi[0],phi[faces[i].x],phi[faces[i].y],phi[7]))/6;
    return volume;
}
#endif
