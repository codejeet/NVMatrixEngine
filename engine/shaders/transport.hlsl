#include "common.hlsli"
#if HIT_MODE == 1 || FLOAT_ATOMICS
#define NV_SHADER_EXTN_SLOT u31
#define NV_SHADER_EXTN_REGISTER_SPACE space1
#include "nvHLSLExtns.h"
#endif
// Traversal payload is 16 bytes. Differential state is packed separately in the
// raygen continuation; no shading/material/differential structs in hit shaders.
#if HIT_MODE == 2
#define PAYLOAD_ACCESS : read(caller) : write(caller,closesthit,miss)
#define PAYLOAD_ATTRIBUTE [raypayload]
#else
#define PAYLOAD_ACCESS
#define PAYLOAD_ATTRIBUTE
#endif
struct PAYLOAD_ATTRIBUTE Payload {
    float t PAYLOAD_ACCESS;
    uint primitive PAYLOAD_ACCESS;
    uint object PAYLOAD_ACCESS;
    uint bary PAYLOAD_ACCESS;
};
#include "fluid/intersection.hlsli"
#include "fluid/whitewater-intersection.hlsli"
#include "fluid/foam-field.hlsli"
[shader("closesthit")]
void Closest(inout Payload p, BuiltInTriangleIntersectionAttributes a) {
    p.t=RayTCurrent(); p.primitive=PrimitiveIndex(); p.object=InstanceID();
    p.bary=f32tof16(a.barycentrics.x)|(f32tof16(a.barycentrics.y)<<16);
}
[shader("miss")]
void Miss(inout Payload p) { p.t=RayTCurrent(); p.object=0xffffffff; p.primitive=0; p.bary=0; }
Payload trace(float3 origin,float3 direction,uint mask,uint hint,float limit=100,bool visibility=false) {
    if(OpticalControls.x&1)++OpticalRayCount;
    RayDesc ray; ray.Origin=origin; ray.Direction=direction; ray.TMin=EPS; ray.TMax=limit;
    // Visibility needs ANY occluder, not the closest surface or its attributes.
    // An accepted hit leaves object=0 because closest-hit is skipped; Miss writes
    // 0xffffffff. Procedural liquid/whitewater intersections still run normally.
    uint flags=RAY_FLAG_FORCE_OPAQUE|(visibility?(RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH|RAY_FLAG_SKIP_CLOSEST_HIT_SHADER):0);
    Payload p; p.t=limit; p.object=visibility?0:0xffffffff; p.primitive=0; p.bary=0;
#if HIT_MODE == 2
    dx::HitObject h=dx::HitObject::TraceRay(Scene,flags,mask,0,0,0,ray,p);
    if(Controls.z) dx::MaybeReorderThread(h,hint,4);
    dx::HitObject::Invoke(h,p);
#elif HIT_MODE == 1
    NvHitObject h=NvTraceRayHitObject(Scene,flags,mask,0,0,0,ray,p);
    if(Controls.z) NvReorderThread(h,hint,4);
    NvInvokeHitObject(Scene,h,p);
#else
    TraceRay(Scene,flags,mask,0,0,0,ray,p);
#endif
    return p;
}
bool occluded(float3 origin,float3 direction,uint mask,uint hint,float limit) {
    return trace(origin,direction,mask,hint,limit,true).object!=0xffffffff;
}
struct Hit { float3 p,n,local; float2 uv; uint chart,material,object; float foam; };
Hit surface(Payload p,float3 o,float3 d,float cone=0) {
    Hit h=(Hit)0; h.p=o+d*p.t; h.object=p.object;
    Object obj=Objects[p.object];
    if(obj.info.z==8){h.local=h.p;h.n=liquidNormal(h.p);h.material=8;h.chart=0xffffffff;
        h.foam=foamCoverage(h.p,cone/max(.2,abs(dot(h.n,d))));return h;}
    if(obj.info.z==9) {
        WhitewaterParticle p2=WhitewaterParticles[p.primitive];h.local=h.p;
        h.n=normalize(h.p-p2.positionRadius.xyz);h.uv.x=p2.positionRadius.w;
        h.material=10+uint(p2.previousType.w);h.chart=0xffffffff;return h;
    }
    uint index=obj.info.x+p.primitive*3;
    Vertex a=Vertices[index],b=Vertices[index+1],c=Vertices[index+2];
    // Derive barycentrics in fp32 from position, not the compressed hit payload.
    // This keeps atlas UVs and dynamic motion guides free of fp16 quantization.
    h.local=mul(h.p-obj.world[3].xyz,transpose((float3x3)obj.world));
    float3 e=b.position-a.position,f=c.position-a.position,v=h.local-a.position;
    float ee=dot(e,e),ef=dot(e,f),ff=dot(f,f),den=ee*ff-ef*ef;
    float2 bary=float2(dot(v,e)*ff-dot(v,f)*ef,dot(v,f)*ee-dot(v,e)*ef)/den;
    h.uv=a.uv+(b.uv-a.uv)*bary.x+(c.uv-a.uv)*bary.y;
    h.n=normalize(mul(normalize(cross(e,f)),(float3x3)obj.world));
    if(a.material==2)h.n=normalize(mul(normalize(h.local),(float3x3)obj.world));
    if(a.material==8&&h.n.y>.5) {
        float2 k=float2(3.7,2.9),j=float2(-4.3,1.7);
        float2 slope=Water.y?0:.065*cos(dot(h.p.xz,k))*k+.035*cos(dot(h.p.xz,j)+.8)*j;
        h.n=normalize(float3(-slope.x,1,-slope.y));
    }
    h.chart=a.chart; h.material=a.material; return h;
}
float2 uvDifferential(Payload p,float3 dp) {
    Object obj=Objects[p.object]; uint i=obj.info.x+p.primitive*3;
    Vertex a=Vertices[i],b=Vertices[i+1],c=Vertices[i+2];
    float3 e=mul(b.position-a.position,(float3x3)obj.world),f=mul(c.position-a.position,(float3x3)obj.world);
    float ee=dot(e,e),ef=dot(e,f),ff=dot(f,f),den=ee*ff-ef*ef;
    float u=(dot(dp,e)*ff-dot(dp,f)*ef)/den,v=(dot(dp,f)*ee-dot(dp,e)*ef)/den;
    return ((b.uv-a.uv)*u+(c.uv-a.uv)*v)*Atlas.z;
}
uint3 packDifferentials(float3 x,float3 y) {
    // Critical-angle derivatives can exceed fp16's finite range while the ray
    // itself remains valid. Bound and count, never feed inf into footprint math.
    if(any(abs(x)>65504)||any(abs(y)>65504))Stats.InterlockedAdd(28,1);
    return f32tof16(clamp(x,-65504,65504))|(f32tof16(clamp(y,-65504,65504))<<16);
}
void unpackDifferentials(uint3 p,out float3 x,out float3 y) { x=f16tof32(p&65535); y=f16tof32(p>>16); }
float3 intersectDifferential(float3 o,float3 d,float3 n,float3 direction,float t) {
    float3 q=o+d*t; float den=dot(n,direction);
    return q-direction*(dot(n,q)/(abs(den)<1e-6?(den<0?-1e-6:1e-6):den));
}
float3 refractDifferential(float3 dd,float3 n,float3 d,float eta) {
    float ci=-dot(n,d),ct=sqrt(max(1e-8,1-eta*eta*(1-ci*ci))),dc=-dot(n,dd);
    return eta*dd+(eta-eta*eta*ci/ct)*dc*n;
}
#include "interface-media.hlsli"
float3 normalDifferential(Hit h,float3 dp) {
    if(h.material==8&&FluidState.x) {
        float e=FluidMinimumSpacing.w*.5;float3 result=0;
        [unroll]for(uint a=0;a<3;++a){float3 v=0;v[a]=e;result+=(liquidNormal(h.p+v)-liquidNormal(h.p-v))*(dp[a]/(2*e));}
        return result-h.n*dot(result,h.n);
    }
    if(h.material==2)return (dp-h.n*dot(dp,h.n))/.68;
    if(h.material==12||h.material==13)return (dp-h.n*dot(dp,h.n))/max(h.uv.x,.001);
    if(h.material==8&&h.n.y>.5&&!Water.y) {
        float2 k=float2(3.7,2.9),j=float2(-4.3,1.7);
        float2 slope=.065*cos(dot(h.p.xz,k))*k+.035*cos(dot(h.p.xz,j)+.8)*j;
        float2 ds=-.065*sin(dot(h.p.xz,k))*k*dot(k,dp.xz)-.035*sin(dot(h.p.xz,j)+.8)*j*dot(j,dp.xz);
        float3 v=float3(-ds.x,0,-ds.y);
        return (v-h.n*dot(h.n,v))/sqrt(1+dot(slope,slope));
    }
    return 0;
}
[shader("raygeneration")]
void FluidProbeRaygen() {
    uint id=DispatchRaysIndex().x,rng=hash(id+71);
    if(FluidState.w&32)for(uint secondaryId=id;secondaryId<8192;secondaryId+=4096) {
        WhitewaterParticle s=WhitewaterParticles[secondaryId];
        if(s.velocityLife.w<=0)continue;
        if(uint(s.previousType.w)==1) {
            // Foam is a continuous coating of the actual liquid, not an opaque
            // sphere. Validate its field and a real nearby water boundary.
            float coverage=foamCoverage(s.positionRadius.xyz);
            if(!isfinite(coverage)||coverage<0||coverage>1)Stats.InterlockedAdd(152,1);
            // Isolated entrainment markers intentionally have no visible foam.
            // Validate persistent layer data separately from positive coverage.
            float4 layer=foamLayer(s.positionRadius.xyz);
            if(any(!isfinite(layer))||layer.x<0||layer.x>4.001)Stats.InterlockedAdd(152,1);
            float3 n=s.surfaceAge.xyz;float span=FluidMinimumSpacing.w*3;
            Payload fp=trace(s.positionRadius.xyz+n*span,-n,8,0,span*2);
            if(fp.object!=0xffffffff) {
                Hit fh=surface(fp,s.positionRadius.xyz+n*span,-n);
                if(fh.material!=8||!isfinite(fh.foam)||fh.foam<0||fh.foam>1)Stats.InterlockedAdd(152,1);
                Stats.InterlockedAdd(140,1);
            }
            continue;
        }
        float3 direction=float3(-1,0,0),origin=s.positionRadius.xyz-direction*s.positionRadius.w*2;
        for(uint crossing=0;crossing<2;++crossing) {
            Payload p=trace(origin,direction,16,0,s.positionRadius.w*5);
            if(p.object==0xffffffff){Stats.InterlockedAdd(152,1);break;}
            // A neighboring sphere can occlude this probe. Test only isolated
            // boundaries rather than assuming the secondary phase never overlaps.
            if(p.primitive!=secondaryId)break;
            Hit h=surface(p,origin,direction);uint kind=uint(s.previousType.w);
            float expected=s.positionRadius.x+s.positionRadius.w*(crossing?-1:1);
            if(h.material!=10+kind||abs(h.p.x-expected)>1e-5||abs(length(h.n)-1)>1e-5)
                Stats.InterlockedAdd(152,1);
            if(kind!=1) {
                uint next;float ni,nt;interfaceMedia(h,direction,550,next,ni,nt);
                uint inside=kind==2?12:8,outside=kind==2?8:0;
                if(next!=(crossing?outside:inside)||abs(ni-mediumIndex(crossing?inside:outside,550))>1e-6||
                   abs(nt-mediumIndex(crossing?outside:inside,550))>1e-6||!glass(h))Stats.InterlockedAdd(152,1);
            }
            Stats.InterlockedAdd((34+kind)*4,1);origin=h.p+direction*EPS*4;
        }
    }
    if(id<3&&!(FluidState.w&16)) {
        // Independently traverse side/end walls and a continuous corner rail.
        // Catches opaque masks, reversed normals, overlapping-box interfaces,
        // and glass accidentally classified as a diffuse receiver.
        float3 go=id==1?float3(3.7,.6,-5.3):float3(1.4,.6,id==2?-5.0:-3.1);
        float3 gd=id==1?float3(0,0,1):float3(1,0,0);
        uint a=id==1?2:0,number=id==2?2:4;
        float4 positions=id==1?float4(-5.1,-4.9,-1.3,-1.1):float4(1.6,1.8,5.6,5.8);
        if(id==2)positions=float4(1.6,5.8,0,0);
        uint j=0;
        for(uint attempt=0;attempt<12&&j<number;++attempt) {
            Payload gp=trace(go,gd,2,0);
            if(gp.object==0xffffffff){Stats.InterlockedAdd(112,1);break;}
            Hit gh=surface(gp,go,gd);
            go=gh.p+gd*EPS*4;
            // The collision fixture moves the prism into this geometric probe.
            // Ignore its boundaries, not the tank's; no optical bending here.
            if(gh.material!=10)continue;
            uint next;float ni,nt;interfaceMedia(gh,gd,550,next,ni,nt);
            bool entering=(j%2)==0;
            if(gh.material!=10||abs(gh.p[a]-positions[j])>1e-5||
               (dot(gh.n,gd)<0)!=entering || (entering&&(next!=10||nt<1.5)) || (!entering&&(next==10||ni<1.5)))
                Stats.InterlockedAdd(112,1);
            Stats.InterlockedAdd(76,1);
            ++j;
        }
        if(j!=number)Stats.InterlockedAdd(112,1);
    }
    uint axis=id%3;float3 d=0;d[axis]=1;
    float3 extent=float3(FluidBricks.xyz*8)*FluidMinimumSpacing.w;
    float3 o=FluidMinimumSpacing.xyz+extent*float3(random(rng),random(rng),random(rng));
    o[axis]=FluidMinimumSpacing[axis]-.1;
    // Through-rays check both liquid entry and exit, including origins inside
    // later disjoint sheets. Root residual is evaluated by a separate sampler.
    for(uint i=0;i<16;++i) {
        Payload p=trace(o,d,8,0);
        if(i==0&&((FluidState.w>>1)&3)==1) {
            float3 q=o-float3(3.7,1.99,-3.1);float b=dot(q,d),disc=b*b-dot(q,q)+.75*.75;
            // Ignore only the grazing silhouette where the sampled sphere differs
            // from its analytic source; test actual hit distances elsewhere.
            if(disc>.02&&(p.object==0xffffffff||abs(p.t-(-b-sqrt(disc)))>.003))Stats.InterlockedAdd(112,1);
        }
        if(i==0&&((FluidState.w>>1)&3)==2&&axis==1&&abs(o.x-3.7)<.95&&abs(o.z+3.1)<.95)
            if(p.object==0xffffffff||abs(o.y+p.t-1.98)>5e-5)Stats.InterlockedAdd(112,1);
        if(p.object==0xffffffff)return;
        float3 position=o+d*p.t;float phi=liquidSample(position).x;
        Stats.InterlockedAdd(108,1);
        if(!isfinite(phi)||abs(phi)>5e-5||p.t<EPS)Stats.InterlockedAdd(112,1);
        o=position+d*EPS*4;
    }
}
struct Source { float3 o,d,right,up; float2 halfSize; float power,nm; };
Source source(uint i) {
    Source s;
    s.o=LightOrigin.xyz;s.d=LightDirection.xyz;s.right=LightRight.xyz;s.up=LightUp.xyz;
    s.halfSize=float2(LightRight.w,LightUp.w);s.power=Optics.w;s.nm=0;
    if(i==1) {s.halfSize=.025;s.power=Water.z*Medium.z;s.nm=Water.w;}
    if(i==2) {
        s.o=float3(2.40,3.9,-3.90);s.d=normalize(float3(.50,-1,.23));basis(s.d,s.right,s.up);
        if(CameraState.w)s.o=float3(9.6,11.9,-15.6);
        s.halfSize=.035;s.power=Water.x*Water.z*Medium.z;s.nm=Water.w;
    }
    if(i==3) {
        s.o=float3(3.70,4.56,-3.10);s.d=float3(0,-1,0);s.right=float3(1,0,0);s.up=float3(0,0,1);
        s.halfSize=float2(1.32,1.22);s.power=Water.x*Medium.w;s.nm=0;
        if(FluidState.w&16){s.o=float3(0,4.56,1);s.halfSize=float2(5.9,6.9);}
        if(CameraState.w){s.o=float3(0,13.56,4);s.halfSize=float2(23.6,27.6);}
    }
    if(i==4) {
        s.o=FlashlightOrigin.xyz;s.d=FlashlightDirection.xyz;basis(s.d,s.right,s.up);
        s.power=FlashlightOrigin.w;s.nm=0;s.halfSize=0;
    }
    return s;
}
void ledger(uint lane,float watts) { Stats.InterlockedAdd(lane*4,uint(round(max(watts,0)*1048576))); }
void addPower(uint address,float value,bool dynamic) {
    if(value<=0) return;
#if FLOAT_ATOMICS
    if(dynamic)NvInterlockedAddFp32(FluidPhotonSum,address,value);
    else NvInterlockedAddFp32(PhotonSum,address,value);
#else
    uint add=uint(round(value*Atlas.w)),old=dynamic?FluidPhotonSum.Load(address):PhotonSum.Load(address),found;
    // Saturating CAS prevents wrap even if future scenes violate the host bound.
    for(;;) {
        uint sum=old+min(add,0xffffffff-old);
        if(dynamic)FluidPhotonSum.InterlockedCompareExchange(address,old,sum,found);
        else PhotonSum.InterlockedCompareExchange(address,old,sum,found);
        if(found==old) { if(add>0xffffffff-old) Stats.InterlockedAdd(20,1); break; }
        old=found;
    }
#endif
}
void splat(Payload payload,Hit h,float3 dx,float3 dy,float3 power,bool dynamic) {
    float2 a=uvDifferential(payload,dx),b=uvDifferential(payload,dy),center=atlasPixel(h.uv,h.chart);
    float cxx=.30+.25*(a.x*a.x+b.x*b.x),cyy=.30+.25*(a.y*a.y+b.y*b.y),cxy=.25*(a.x*a.y+b.x*b.y);
    // Room-wide water bundles cover much more area than the old pit light.
    // The old two-texel sigma cap cut even a FLAT pool's launch footprint,
    // opening repeating dark gaps between photons. Keep the differential-sized
    // footprint (including focused/thin caustics), with a bounded four-texel
    // sigma only for excessive dynamic/grazing footprints. Static prism paths
    // retain their existing cap. Per-photon discrete normalization below is vital.
    float scale=min(1.0,(dynamic?16.0:4.0)/max(cxx,cyy));
    if(scale<1) Stats.InterlockedAdd(28,1);
    // Preserve the reconstruction-kernel floor AFTER shrinking large footprints.
    // Shrinking it too made near-critical ellipses numerically singular/empty.
    cxx=.30+(cxx-.30)*scale; cyy=.30+(cyy-.30)*scale; cxy*=scale;
    float det=max(cxx*cyy-cxy*cxy,1e-8);
    int2 lo=max(int2(floor(center-3*sqrt(float2(cxx,cyy)))),int2(h.chart*uint(Atlas.z),0));
    int2 hi=min(int2(ceil(center+3*sqrt(float2(cxx,cyy)))),int2((h.chart+1)*uint(Atlas.z)-1,uint(Atlas.y)-1));
    float norm=0;
    for(int y=lo.y;y<=hi.y;++y) for(int x=lo.x;x<=hi.x;++x) {
        float2 q=float2(x,y)-center; float r=(cyy*q.x*q.x-2*cxy*q.x*q.y+cxx*q.y*q.y)/det;
        if(r<=9) norm+=exp(-.5*r);
    }
    if(norm<=0||any(!isfinite(power))) { Stats.InterlockedAdd(24,1); return; }
    for(int y=lo.y;y<=hi.y;++y) for(int x=lo.x;x<=hi.x;++x) {
        float2 q=float2(x,y)-center; float r=(cyy*q.x*q.x-2*cxy*q.x*q.y+cxx*q.y*q.y)/det;
        if(r>9) continue;
        float3 value=power*(exp(-.5*r)/norm); uint address=(y*uint(Atlas.x)+x)*12;
        addPower(address,value.x,dynamic); addPower(address+4,value.y,dynamic); addPower(address+8,value.z,dynamic);
    }
    Stats.InterlockedAdd(12,1);
}
// First two Sobol dimensions; per-frame digital shifts preserve stratification.
#include "photon-sampling.hlsli"
float3 sobol(uint i,uint frame) {
    uint x=0,y=0,z=0,vx=0x80000000,vy=0x80000000,vz=0x80000000,previousZ=0,g=i^(i>>1);
    for(uint bit=0;bit<32;++bit) {
        if(g&(1u<<bit)) { x^=vx; y^=vy; z^=vz; }
        vx>>=1; vy^=vy>>1;
        uint nextZ=bit==0?0xc0000000:(previousZ^(previousZ>>2)^vz);
        previousZ=vz; vz=nextZ;
    }
    return float3((x^hash(frame*3+1))>>8,(y^hash(frame*3+2))>>8,(z^hash(frame*3+3))>>8)*(1.0/16777216.0);
}
[shader("raygeneration")]
void PhotonRaygen() {
    uint index=DispatchRaysIndex().x;Stats.InterlockedAdd(0,1);
    uint light=0,local=index,count=Dimensions.w;
    if(Play.x) {
        uint n4=FlashlightOrigin.w>0?(Lighting.w==3?Dimensions.w:Dimensions.w/4):0;
        uint budget=Dimensions.w-n4;
        uint n1=Water.z?budget/8:0,n2=Water.x&&Water.z?budget/8:0;
        uint n3=Water.x?budget/(Water.z?4:2):0;
        if(Lighting.w>0)n3=budget;
        uint n0=budget-n1-n2-n3;
        if(index<n0){count=n0;}
        else if(index<n0+n1){light=1;local=index-n0;count=n1;}
        else if(index<n0+n1+n2){light=2;local=index-n0-n1;count=n2;}
        else if(index<budget){light=3;local=index-budget+n3;count=n3;}
        else {light=4;local=index-budget;count=n4;}
    }
    Source s=source(light);if(s.power<=0||!count)return;
    // Correlated eight-wavelength bundles share water-flood aperture samples.
    // Each lane traces its own spectral IOR and Fresnel events. Uniform spectral
    // marginals and power/count normalization are unchanged; no RGB desaturation.
    uint bundle=FluidState.x&&light==3&&count%8==0?8u:1u;
    uint spatialIndex=local/bundle,lane=local%bundle;
    uint rng=hash((bundle>1?spatialIndex:index)+Dimensions.z*0x517cc1b7);
    float3 sample=sobol(spatialIndex,Dimensions.z+light*137);
    if(bundle>1) {
        // All wavelengths still share one launch point and Fresnel random stream.
        // Scramble aperture ONLY: retain stratified spectral marginals/energy.
        sample.x=scramblePhotonAperture(sample.x,hash(Dimensions.z*2+0x19b2u));
        sample.y=scramblePhotonAperture(sample.y,hash(Dimensions.z*2+0x93d7u));
        sample.z=(lane+sample.z)/bundle;
    }
    float nm=s.nm>0?s.nm:380+400*min(sample.z+random(rng)*(1.0/16777216.0),.99999999);
    float2 aperture=(sample.xy*2-1)*s.halfSize;
    if(s.nm>0)aperture=sqrt(sample.x)*s.halfSize.x*float2(cos(2*PI*sample.y),sin(2*PI*sample.y));
    float3 o=s.o+s.right*aperture.x+s.up*aperture.y,d=s.d;
    float cell=s.nm>0?sqrt(PI):2;
    uint3 op=packDifferentials(s.right*(cell*s.halfSize.x/sqrt(float(count)/bundle)),s.up*(cell*s.halfSize.y/sqrt(float(count)/bundle)));
    uint3 dp=0;
    if(light==4) {
        float cosine=lerp(1,FlashlightDirection.w,sample.x),sine=sqrt(max(0,1-cosine*cosine));
        d=normalize(s.d*cosine+(s.right*cos(2*PI*sample.y)+s.up*sin(2*PI*sample.y))*sine);
        float3 dx,dy;basis(d,dx,dy);
        float spread=sqrt(2*PI*(1-FlashlightDirection.w)/count);
        op=0;dp=packDifferentials(dx*spread,dy*spread);
    }
    float energy=s.power/count;uint band=min(uint((nm-380)/25),15u),medium=ambientMedium(o);
    ledger(20,energy);
    Payload p;
    if(s.nm>0)p=trace(o,d,255,band);
    else {
        // Broad collimators own refracted transport; their un-refracted direct
        // complement is evaluated by NEE, so it is not splatted a second time.
        p=trace(o,d,14,band);
        if(p.object==0xffffffff){ledger(23,energy);return;}
        if(occluded(o,d,1,band,max(EPS*2,p.t-EPS))){ledger(24,energy);return;}
    }
    bool entered=false,dynamic=false;
    for(uint event=0;event<12;++event) {
        if(event>0)p=trace(o,d,255,band);
        if(p.object==0xffffffff){ledger(23,energy);return;}
        float before=energy;energy*=exp(-extinction(medium,nm)*p.t);ledger(22,before-energy);
        Hit h=surface(p,o,d);
        dynamic=dynamic||(FluidState.x&&h.material==8);
        float3 ox,oy,ddx,ddy;unpackDifferentials(op,ox,oy);unpackDifferentials(dp,ddx,ddy);
        ox=intersectDifferential(ox,ddx,h.n,d,p.t);oy=intersectDifferential(oy,ddy,h.n,d,p.t);
        if(!glass(h)) {
            if((entered||s.nm>0)&&h.chart<5) {
                splat(p,h,ox,oy,spectralPower(nm,energy),dynamic);ledger(21,energy);
                if(s.nm>0)Stats.InterlockedAdd(64,1);
                if(h.chart==4)ledger(15,energy);
                if(Play.x&&h.chart==3&&abs(h.p.y-Sensor.y)<.65&&abs(h.p.z-Sensor.z)<Sensor.w&&nm>=510&&nm<=570)
                    Stats.InterlockedAdd(48,uint(round(energy*1048576)));
            } else ledger(24,energy);
            return;
        }
        if(!entered)Stats.InterlockedAdd(4,1);
        uint next;float ni,nt;interfaceMedia(h,d,nm,next,ni,nt);
        float3 normal=dot(h.n,d)<0?h.n:-h.n;float eta=ni/nt;
        float side=dot(h.n,d)<0?1:-1;
        float3 nx=side*normalDifferential(h,ox),ny=side*normalDifferential(h,oy);
        float F=fresnel(saturate(-dot(normal,d)),ni,nt);
        bool reflected=F>=1||random(rng)<F;
        if(light==0&&!entered) {
            // Preserve the original collimator's transmission-only entrance class.
            ledger(26,energy*F);energy*=1-F;reflected=false;
            if(F>=1)return;
        }
        if(reflected) {
            ddx=reflect(ddx,normal)-2*(dot(d,nx)*normal+dot(d,normal)*nx);
            ddy=reflect(ddy,normal)-2*(dot(d,ny)*normal+dot(d,normal)*ny);
            d=reflect(d,normal);
        } else {
            // Effective microbubble scattering removes this fraction from the
            // specular chain. Do not send its energy through the covered water
            // and paint a full-strength caustic underneath an opaque foam raft.
            float foamLoss=energy*h.foam;energy-=foamLoss;ledger(24,foamLoss);
            if(energy<=0)return;
            float ci=-dot(normal,d),ct=sqrt(max(1e-8,1-eta*eta*(1-ci*ci)));
            ddx=refractDifferential(ddx,normal,d,eta)-(eta-eta*eta*ci/ct)*dot(nx,d)*normal+(eta*ci-ct)*nx;
            ddy=refractDifferential(ddy,normal,d,eta)-(eta-eta*eta*ci/ct)*dot(ny,d)*normal+(eta*ci-ct)*ny;
            d=normalize(refract(d,normal,eta));
            if(next==8&&medium!=8)Stats.InterlockedAdd(56,1);
            if(next==0)Stats.InterlockedAdd(8,1);
            medium=next;entered=true;
        }
        o=h.p+d*EPS*2;op=packDifferentials(ox,oy);dp=packDifferentials(ddx,ddy);
    }
    ledger(25,energy);Stats.InterlockedAdd(16,1);
}
#include "lasers.hlsli"
float3 directLight(Hit h,float3 n,inout uint rng,uint areaSamples=1) {
    // Shared local-light NEE oracle; used by baseline and RTXDI PT paths.
    float3 result=laserIrradiance(h,n);
    if(FlashlightOrigin.w>0) {
        float3 delta=FlashlightOrigin.xyz-h.p;float distance=length(delta);float3 wi=delta/max(distance,EPS);
        float cosine=max(dot(n,wi),0);
        if(cosine>0&&dot(-wi,FlashlightDirection.xyz)>=FlashlightDirection.w&&
           !occluded(h.p+n*EPS*2,wi,255,4,distance-EPS*4))
            result+=FlashlightOrigin.w*cosine/(2*PI*(1-FlashlightDirection.w)*max(distance*distance,1e-5))*
                    exp(-extinctionRgb(ambientMedium(h.p))*distance);
    }
    const float3 positions[2]={float3(-3,4,-3),float3(4,3,3)};
    const float3 powers[2]={float3(25,4,38),float3(3,25,32)};
    for(uint i=0;i<2&&Lighting.x>0;++i) {
        float3 position=positions[i];
        if(CameraState.w)position=position*float3(4,1,4)+float3(0,8,0);
        float3 delta=position-h.p; float dist=length(delta),cosine=max(dot(n,delta/dist),0);
        if(cosine>0 && !occluded(h.p+n*EPS*2,delta/dist,255,i,dist-EPS*4))
            result+=(Lighting.w==2?float3(80,80,80):powers[i]*(Play.x?8:1))*cosine/(dist*dist*4*PI)*exp(-extinctionRgb(ambientMedium(h.p))*dist);
    }
    if(Play.x) {
        // Actual moving cube emitters: sample a visible face by projected center
        // area, then a uniform point on it, retaining the face/area PDF.
        // Use the pixel/path stream, not floating-point hit-position bits. Camera
        // jitter must not unpredictably reseed all area-light samples at a vertex.
        for(uint object=3;object<=4&&Lighting.y>0;++object) {
            if(object==h.object)continue;
            Object obj=Objects[object];
            float3 local=mul(h.p-obj.world[3].xyz,transpose((float3x3)obj.world));
            // Only faces actually visible from the shading point have nonzero cosine.
            float3 areaWeight=max(abs(local)-.38,0);
            float sum=areaWeight.x+areaWeight.y+areaWeight.z;
            if(sum<1e-6)continue;
            for(uint sampleIndex=0;sampleIndex<areaSamples;++sampleIndex){
            float choice=random(rng)*sum;
            uint axis=choice<areaWeight.x?0:(choice<areaWeight.x+areaWeight.y?1:2);
            float3 ln=0;ln[axis]=local[axis]>0?1:-1;
            float3 localSample=0;localSample[axis]=ln[axis]*.38;
            localSample[(axis+1)%3]=(random(rng)*2-1)*.38;localSample[(axis+2)%3]=(random(rng)*2-1)*.38;
            float3 worldPoint=mul(float4(localSample,1),obj.world).xyz;
            float3 lightNormal=mul(ln,(float3x3)obj.world);
            float3 delta=worldPoint-h.p;float distance=length(delta);
            float3 dir=delta/distance;float cosine=max(dot(n,dir),0);
            float cosineLight=max(dot(lightNormal,-dir),0),pdf=areaWeight[axis]/(sum*.76*.76);
            if(cosine>0&&cosineLight>0&&!occluded(h.p+n*EPS*2,dir,255,object,distance-EPS*4))
                result+=(object==3?float3(5,.12,2.8):float3(.08,3.8,5))*cosine*cosineLight/(distance*distance*pdf*areaSamples)*exp(-Medium.x*distance);
            }
        }
        // Complementary un-refracted collimated illumination. Visibility rejects
        // any glass hit; refracted source energy is already owned by the atlas.
        float t=dot(h.p-LightOrigin.xyz,LightDirection.xyz);
        float3 launch=h.p-LightDirection.xyz*t-LightOrigin.xyz;
        if(t>0&&abs(dot(launch,LightRight.xyz))<LightRight.w&&abs(dot(launch,LightUp.xyz))<LightUp.w&&
           !occluded(h.p+n*EPS*2,-LightDirection.xyz,255,0,t-EPS*4))
            result+=Optics.w/(4*LightRight.w*LightUp.w)*max(dot(n,-LightDirection.xyz),0)*exp(-Medium.x*t);
        // Matching direct complement of the overhead pool collimator. This was
        // missing outside its refracted photon paths; no photons double count it.
        if(Water.x){
            Source flood=source(3);float distance=flood.o.y-h.p.y;
            float2 q=abs(h.p.xz-flood.o.xz);
            if(distance>0&&all(q<flood.halfSize)&&dot(n,-flood.d)>0&&
               !occluded(h.p+n*EPS*2,-flood.d,255,3,distance-EPS*4))
                result+=flood.power/(4*flood.halfSize.x*flood.halfSize.y)*dot(n,-flood.d)*exp(-Medium.x*distance);
        }
    }
    return result;
}
float3 baseColor(Hit h,float2 footprint=0) {
    if(h.material==15)return float3(.82,.88,.86);
    if(h.material==16)return float3(.95,.24,.035);
    if(h.material==17)return float3(.04,.055,.065);
    if(h.material==11)return float3(.86,.89,.90);
    if(h.chart<5)return receiverAlbedo(h.chart,h.uv,footprint);
    return h.material==3?float3(.12,.16,.21):float3(.5,.5,.5);
}
float3 emission(Hit h) {
    if(!Lighting.y)return 0;
    if(h.material==14)return Play.w?float3(.1,3.5,1.0):float3(.2,.5,.65);
    if(h.material==9)return Water.z?xyzToRgb(spectralPower(Water.w,3)):0;
    if(h.material==4)return float3(5,.12,2.8);
    if(h.material==5)return float3(.08,3.8,5);
    if(h.material==6)return float3(5,4.5,3.4);
    // A real low-output calibration plate under the glass makes refraction legible
    // in overhead precision mode; this emission belongs to the cradle, not glass.
    if(h.material==7) {
        float2 grid=abs(frac(h.p.xz*8)-.5);
        return max(grid.x,grid.y)>.45?float3(.15,.5,.58):float3(.035,.07,.085);
    }
    if(Play.x&&h.chart==3) {
        float2 q=abs(float2(h.p.y-Sensor.y,h.p.z-Sensor.z));
        if(all(q<float2(.70,Sensor.w+.05))&&any(q>float2(.65,Sensor.w)))
            return lerp(float3(.06,.5,.25),float3(.2,4,1.3),Play.y);
    }
    return 0;
}
float3 sky(float3 d) {
    return Lighting.z*lerp(float3(.006,.009,.021),float3(.026,.048,.08),saturate(d.y*.5+.5));
}
float2 cameraReceiverFootprint(Payload payload,Hit h,float3 direction,float opticalDistance) {
    if(h.chart>=5)return 0;
    // A pixel cone projected onto the receiver. Accumulated optical distance
    // includes specular/refraction segments, rather than pretending the floor is
    // at the water interface. No derivative intrinsics or additional traced rays.
    float diameter=2*CameraUp.w/Dimensions.y*opticalDistance;
    float3 x,y;basis(direction,x,y);
    x=intersectDifferential(x*diameter,0,h.n,direction,0);
    y=intersectDifferential(y*diameter,0,h.n,direction,0);
    return (abs(uvDifferential(payload,x))+abs(uvDifferential(payload,y)))/Atlas.z;
}
float3 endpoint(Hit h,float3 n,inout uint rng,float2 footprint=0,uint areaSamples=1) {
    opticalRecordReceiver(h.uv,h.chart);
    return emission(h)+baseColor(h,footprint)*(directLight(h,n,rng,areaSamples)+causticIrradiance(h.uv,h.chart))/PI;
}
// Whitted-style specular prefix only (E S* D / E S* L), before any diffuse event.
// Branch both Fresnel lobes, including repeated internal reflections and exact TIR.
// The camera uses the 550 nm reference IOR for broadband appearance; the photon
// estimator remains wavelength-resolved. This is NOT a spectral camera estimator.
float3 dielectricView(float3 origin,float3 direction,bool primaryGlass,Payload primaryPayload,inout uint rng,out float hitDistance,out uint3 events,float initialDistance=0,uint areaSamples=1) {
    float3 origins[12],directions[12],weights[12];uint depths[12],media[12];
    float opticalDistances[12];opticalDistances[0]=initialDistance;
    float cameraIor=mediumIndex(ambientMedium(CameraPosition.xyz),550);
    origins[0]=origin;directions[0]=direction;weights[0]=1;depths[0]=0;media[0]=ambientMedium(origin);
    uint pending=1;float3 result=0;events=0;hitDistance=0;
    for(uint visit=0;visit<48&&pending>0;++visit) {
        uint k=--pending,depth=depths[k],medium=media[k];
        float3 o=origins[k],d=directions[k],weight=weights[k];
        Payload p;
        if(visit==0&&primaryGlass&&!(OpticalControls.w&512))p=primaryPayload;
        else p=trace(o,d,255,depth&15);
        // Paraxial cone propagation; exact for planar, normal-incidence media.
        // Curved-interface magnification is not a full ray-differential solution.
        float opticalDistance=opticalDistances[k]+p.t*cameraIor/mediumIndex(medium,550);
        // FIRST reflected segment, not a terminal distance after transmission.
        if(visit==(primaryGlass?1:0))hitDistance=p.object==0xffffffff?0:p.t;
        result+=weight*beamRadiance(o,d,p.t,medium);
        weight*=exp(-extinctionRgb(medium)*p.t);
        if(p.object==0xffffffff){result+=weight*sky(d);continue;}
        Hit h=surface(p,o,d,2*CameraUp.w/Dimensions.y*opticalDistance);float3 normal=dot(h.n,d)<0?h.n:-h.n;
        if(!glass(h)){result+=weight*endpoint(h,normal,rng,cameraReceiverFootprint(p,h,d,opticalDistance),areaSamples);continue;}
        if(depth>=10){Stats.InterlockedAdd(52,1);continue;}
        uint next;float ni,nt;interfaceMedia(h,d,550,next,ni,nt);
        float eta=ni/nt,F=fresnel(saturate(-dot(normal,d)),ni,nt);
        float3 transmitted=refract(d,normal,eta);
        if(F>=1)events.y++;
        float3 foamWeight=weight*(1-F)*h.foam*FoamReflectance;
        // Apply the existing bounded-prefix throughput cutoff to the diffuse
        // lobe too. Tiny reflected coating tails otherwise trace a full set of
        // light visibility rays even after their contribution is negligible.
        if(max(foamWeight.x,max(foamWeight.y,foamWeight.z))>.001) {
            Hit coating=h;coating.p+=normal*EPS*4;
            result+=foamWeight*directLight(coating,normal,rng,areaSamples)/PI;
        }
        // Radiance eta^2 cancels through a complete slab; photon power never gets it.
        float3 tw=weight*(1-F)*(1-h.foam)*eta*eta;
        if(max(tw.x,max(tw.y,tw.z))>.001&&pending<12) {
            float3 td=normalize(transmitted);
            origins[pending]=h.p+td*EPS*2;directions[pending]=td;weights[pending]=tw;
            opticalDistances[pending]=opticalDistance;
            media[pending]=next;depths[pending++]=depth+1;events.x++;
        }
        float3 rw=weight*F;
        if(max(rw.x,max(rw.y,rw.z))>.001&&pending<12) {
            float3 rd=reflect(d,normal);
            origins[pending]=h.p+rd*EPS*2;directions[pending]=rd;weights[pending]=rw;
            opticalDistances[pending]=opticalDistance;
            media[pending]=medium;depths[pending++]=depth+1;events.z++;
        }
    }
    if(pending)Stats.InterlockedAdd(52,pending);
    return result;
}
#include "restir-pt.hlsli"
[shader("raygeneration")]
void CameraRaygen() {
    uint2 pixel=DispatchRaysIndex().xy;
    if(PTControls.x)ptInitialize(pixel);
    float2 clip=((pixel+.5+Jitter.xy)/Dimensions.xy)*float2(2,-2)+float2(-1,1);
    float3 d=normalize(CameraForward.xyz+CameraRight.xyz*(clip.x*CameraRight.w)+CameraUp.xyz*(clip.y*CameraUp.w));
    float3 o=CameraPosition.xyz,throughput=1,radiance=0;
    uint rng=hash(pixel.x+pixel.y*Dimensions.x+Dimensions.z*0x517cc1b7);
    Payload p=trace(o,d,CameraState.x?251:255,0);
    Surface[pixel]=0; Motion[pixel]=0; Depth[pixel]=200;
    NormalRoughness[pixel]=float4(-d,1); Albedo[pixel]=0; SpecularAlbedo[pixel]=0; SpecularDistance[pixel]=0;
    if(p.object!=0xffffffff) {
        Hit primary=surface(p,o,d,2*CameraUp.w/Dimensions.y*p.t); float3 n=dot(primary.n,d)<0?primary.n:-primary.n;
        float3 old=mul(float4(primary.local,1),Objects[p.object].previous).xyz;
        if(Objects[p.object].info.z==8&&liquidSurfaceMoving(FluidState.y))old+=liquidSample(primary.p).yzw;
        if(Objects[p.object].info.z==9) {
            WhitewaterParticle s=WhitewaterParticles[p.primitive];old+=s.previousType.xyz-s.positionRadius.xyz;
        }
        float4 prev=mul(float4(old,1),PreviousViewProjection),now=mul(float4(primary.p,1),ViewProjection);
        Motion[pixel]=(prev.xy/prev.w-now.xy/now.w)*float2(.5,-.5)*Dimensions.xy;
        Depth[pixel]=max(.05,dot(primary.p-o,CameraForward.xyz));
        NormalRoughness[pixel]=float4(n,glass(primary)?.015:(Play.x&&primary.chart==0?.025:1));
        uint pixelSamples=opticalBegin(pixel,primary.p,old,n,NormalRoughness[pixel].w,p.object,primary.material,
            Objects[p.object].info.z==8?p.primitive:0xffffffff);
        if(Objects[p.object].info.z==8&&(FluidState.w>>8)) {
            uint mode=FluidState.w>>8;
            uint id=hash(p.primitive);float3 color=float3(id&255,(id>>8)&255,(id>>16)&255)/255;
            if(mode==1)color=primary.n*.5+.5;
            if(mode==3)color=saturate(float3(Motion[pixel]*.05+.5,.5));
            if(mode==4){uint slot=LiquidBrickMap[p.primitive];
                float blend=float(LiquidBrickMap[FluidBricks.w+slot*17+16]>>24)/255;
                color=lerp(float3(1,.3,.05),float3(.05,.4,1),blend);}
            Noisy[pixel]=float4(color,1);Albedo[pixel]=1;NormalRoughness[pixel].w=1;opticalEndCamera(pixel);return;
        }
        if(glass(primary)) {
            uint next;float ni,nt;interfaceMedia(primary,d,550,next,ni,nt);
            float F=fresnel(abs(dot(n,d)),ni,nt); SpecularAlbedo[pixel]=float4(F.xxx,1);
            // Keep the wet-film specular guide while exposing the foam's
            // genuinely diffuse reflectance to RR. No white emissive cheat.
            Albedo[pixel]=float4(primary.foam*(1-F)*FoamReflectance,1);
            float hitDistance;uint3 events;
            radiance=dielectricView(o,d,true,p,rng,hitDistance,events,0,pixelSamples);
            SpecularDistance[pixel]=min(hitDistance,200);
            Stats.InterlockedAdd(32,1);Stats.InterlockedAdd(36,events.x);Stats.InterlockedAdd(40,events.y);Stats.InterlockedAdd(44,events.z);
        } else {
            float3 albedo=baseColor(primary,cameraReceiverFootprint(p,primary,d,p.t));
            Albedo[pixel]=float4(albedo,1);
            if(primary.chart<5)Surface[pixel]=float4(atlasPixel(primary.uv,primary.chart),primary.chart,p.t);
            radiance=emission(primary)+directLight(primary,n,rng,pixelSamples)*albedo/PI;
            if(Play.x&&primary.chart==0) {
                // A small polished-floor lobe carries reflected glass/neon appearance.
                // Its caustics endpoint lookup observes the same atlas, never re-traces L S D.
                float distance;uint3 events;
                float3 reflected=dielectricView(primary.p+n*EPS*2,reflect(d,n),false,p,rng,distance,events,p.t,pixelSamples);
                float f=.07+.93*pow(1-saturate(-dot(n,d)),5);
                radiance=lerp(radiance,reflected,f);albedo*=1-f;Albedo[pixel]=float4(albedo,1);
                SpecularAlbedo[pixel]=float4(f.xxx,1);SpecularDistance[pixel]=min(distance,200);
            }
            // Raw 2-bounce diffuse baseline. No screen-space temporal filtering.
            if(PTControls.x)ptInitial(pixel,primary,n,-normalize(primary.p-CameraPosition.xyz),albedo);
            if(!PTControls.x){
            if(OpticalControls.x&1)OpticalInitialPaths+=pixelSamples;
            for(uint sampleIndex=0;sampleIndex<pixelSamples;++sampleIndex){
            throughput=albedo/float(pixelSamples);
            o=primary.p+n*EPS*2; d=diffuseDirection(n,rng);
            for(uint bounce=0;bounce<2;++bounce) {
                Payload secondary=trace(o,d,255,bounce+1);
                throughput*=exp(-extinctionRgb(ambientMedium(o))*secondary.t);
                if(secondary.object==0xffffffff) { radiance+=throughput*sky(d); break; }
                Hit h=surface(secondary,o,d);
                if(glass(h)) break; // After a diffuse event, photons exclusively own refractive caustics.
                float3 normal=dot(h.n,d)<0?h.n:-h.n;
                float3 a=baseColor(h);
                radiance+=throughput*endpoint(h,normal,rng);
                // Cube area lights were already explicitly sampled at the previous
                // diffuse vertex. Do not count their BSDF-hit emission a second time.
                if(Play.x&&(h.object==3||h.object==4))radiance-=throughput*emission(h);
                throughput*=a; o=h.p+normal*EPS*2; d=diffuseDirection(normal,rng);
            }
            }}
            float3 cameraDirection=normalize(primary.p-CameraPosition.xyz);
            uint viewMedium=ambientMedium(CameraPosition.xyz);
            radiance=radiance*exp(-extinctionRgb(viewMedium)*p.t)+beamRadiance(CameraPosition.xyz,cameraDirection,p.t,viewMedium);
        }
    } else {
        opticalBegin(pixel,o+d*200,o+d*200,-d,1,0xffffffff,0xffffffff);
        uint medium=ambientMedium(o);
        radiance=sky(d)*exp(-extinctionRgb(medium)*p.t)+beamRadiance(o,d,p.t,medium);
        float4 prev=mul(float4(o+d*200,1),PreviousViewProjection),now=mul(float4(o+d*200,1),ViewProjection);
        Motion[pixel]=(prev.xy/prev.w-now.xy/now.w)*float2(.5,-.5)*Dimensions.xy;
    }
    Noisy[pixel]=float4(radiance,1);
    opticalEndCamera(pixel);
}
