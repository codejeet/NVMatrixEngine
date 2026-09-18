#ifndef LAB_FOAM_FIELD
#define LAB_FOAM_FIELD
// Persistent layer: optical depth + density-weighted material displacement.
// Markers supply entrainment only; no individual marker is a visible primitive.
StructuredBuffer<float4> SurfaceFoam : register(t8);
uint foamGrainHash(uint s){s^=s>>16;s*=0x7feb352d;s^=s>>15;s*=0x846ca68b;return s^(s>>16);}
float foamGrainValue(int2 p,uint seed) {
    uint2 key=asuint(p);
    return float(foamGrainHash(key.x^foamGrainHash(key.y+seed))>>8)*(2.0/16777216)-1;
}
float foamGrainNoise(float2 p,uint seed) {
    int2 base=int2(floor(p));float2 f=frac(p);
    // Quintic interpolation hides lattice seams without constructing rings,
    // nearest-point borders or repeated bubble shapes.
    f=f*f*f*(f*(f*6-15)+10);
    return lerp(lerp(foamGrainValue(base,seed),foamGrainValue(base+int2(1,0),seed),f.x),
                lerp(foamGrainValue(base+int2(0,1),seed),foamGrainValue(base+1,seed),f.x),f.y);
}
float foamGrain(float2 material,float footprint) {
    const float scales[4]={.071,.023,.0083,.0031};
    const float weights[4]={.15,.50,.25,.10};
    float2 p=float2(.932327*material.x-.361615*material.y,.361615*material.x+.932327*material.y);
    float grain=0;
    [unroll]for(uint band=0;band<4;++band){
        float resolved=1-smoothstep(.2,.85,footprint/scales[band]);
        // Independently seeded, rotated bands at non-doubling scales. Filter
        // each unresolved band to its zero mean, rather than adding frame noise.
        if(resolved>0)grain+=weights[band]*resolved*foamGrainNoise(p/scales[band],8927+band*1013);
        p=float2(.8*p.x-.6*p.y,.6*p.x+.8*p.y);
    }
    return grain;
}
float4 foamLayer(float3 world) {
    if(!FluidState.x||!(FluidState.w&32))return 0;
    float3 g=(world-FluidMinimumSpacing.xyz)/FluidMinimumSpacing.w;
    uint3 size=FluidBricks.xyz*8+1;
    if(any(g<0)||any(g>=float3(size-1)))return 0;
    uint3 p=uint3(floor(g));float3 f=frac(g);float4 value=0;
    [unroll]for(uint k=0;k<8;++k){uint3 b=uint3(k&1,(k>>1)&1,k>>2),q=p+b;
        float3 w=lerp(1-f,f,float3(b));
        value+=SurfaceFoam[(q.z*size.y+q.y)*size.x+q.x]*w.x*w.y*w.z;}
    return value;
}
float foamCoverage(float3 world,float footprint=0) {
    float4 layer=foamLayer(world);float density=layer.x;
    if(density<=.1)return 0;
    float3 material=world+layer.yzw/density;
    // Subgrid foam gathers into irregular rafts with open water between them.
    // Advect those rafts with the simulated layer, and filter them at distance.
    // A metre-scale source grid must not appear as a smooth painted white tube.
    float rafts=.65*(1-smoothstep(.08,.5,footprint))*foamGrainNoise(material.xz/.47,1931)
               +.35*(1-smoothstep(.25,1.2,footprint))*foamGrainNoise(material.xz/1.37,7193);
    density*=max(0,1+2.8*rafts);
    float envelope=smoothstep(.1,.75,density)*(1-exp(-density));
    // Preserve the previous layer's average coverage, but replace its visible
    // cell structure with bounded, zero-mean irregular grain. No hard cutouts.
    float scatter=.1219625+density*(.06671875-.00146484375*density);
    float wet=lerp(.04,.35,smoothstep(1.5,4,density));
    float mean=lerp(wet,1,scatter);
    float variation=.85*min(mean,1-mean)*foamGrain(material.xz,footprint);
    return envelope*(mean+variation);
}
static const float3 FoamReflectance=float3(.86,.89,.88);
#endif
