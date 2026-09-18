#ifndef OCEAN_ENVIRONMENT
#define OCEAN_ENVIRONMENT
StructuredBuffer<float4> OceanEnvironment : register(t9);
uint oceanLayer(){return uint(Lighting.w)>=5?1:0;}
uint oceanEnvironmentBase(){uint2 size=uint2(OceanEnvironment[0].xy);return 4+oceanLayer()*(size.x*size.y+size.y);}
float4 oceanTexel(int2 p) {
    int2 size=int2(OceanEnvironment[0].xy);
    p.x=(p.x%size.x+size.x)%size.x;p.y=clamp(p.y,0,size.y-1);
    return OceanEnvironment[oceanEnvironmentBase()+p.y*size.x+p.x];
}
float3 oceanSky(float3 d) {
    float2 uv=float2(frac(atan2(d.z,d.x)/(2*PI)+1),acos(clamp(d.y,-1,1))/PI);
    float2 p=uv*OceanEnvironment[0].xy-.5,f=frac(p);int2 q=int2(floor(p));
    return lerp(lerp(oceanTexel(q).rgb,oceanTexel(q+int2(1,0)).rgb,f.x),
                lerp(oceanTexel(q+int2(0,1)).rgb,oceanTexel(q+1).rgb,f.x),f.y);
}
float3 oceanEnvironmentSample(inout uint rng,out float3 direction,out float pdf) {
    uint2 size=uint2(OceanEnvironment[0].xy);uint count=size.x*size.y,base=oceanEnvironmentBase();
    float xi=random(rng);uint lo=0,hi=size.y-1;
    while(lo<hi){uint mid=(lo+hi)/2;if(OceanEnvironment[base+count+mid].x<=xi)lo=mid+1;else hi=mid;}
    uint2 q=uint2(0,lo);
    float probability=OceanEnvironment[base+count+lo].x-(lo?OceanEnvironment[base+count+lo-1].x:0);
    uint row=base+lo*size.x;
    xi=random(rng);lo=0;hi=size.x-1;
    while(lo<hi){uint mid=(lo+hi)/2;if(OceanEnvironment[row+mid].w<=xi)lo=mid+1;else hi=mid;}
    q.x=lo;
    probability*=OceanEnvironment[row+lo].w-(lo?OceanEnvironment[row+lo-1].w:0);
    float c0=cos(PI*q.y/size.y),c1=cos(PI*(q.y+1)/size.y);
    float cosine=lerp(c0,c1,random(rng)),sine=sqrt(max(0,1-cosine*cosine));
    float phi=2*PI*(q.x+random(rng))/size.x;
    direction=float3(sine*cos(phi),cosine,sine*sin(phi));
    pdf=probability/max(1e-12,(2*PI/size.x)*(c0-c1));
    return oceanSky(direction);
}
#endif
