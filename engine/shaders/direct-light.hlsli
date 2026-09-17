#include "lambertian-sampling.hlsli"
struct DiffuseLightSample { float3 direction,irradiance; float distance; uint hint; };

// Five delta sources followed by two cube samples per area-sample stratum.
// Each candidate has its own seed so inclusion decisions do not consume area
// sample dimensions or perturb the parent path stream.
DiffuseLightSample diffuseLightSample(Hit h,float3 n,float3 sigma,uint candidate,uint seed,uint areaSamples) {
    DiffuseLightSample s=(DiffuseLightSample)0;
    float3 delta=0,power=0;float scale=0;
    if(candidate==0) {
        if(FlashlightOrigin.w<=0)return s;
        delta=FlashlightOrigin.xyz-h.p;s.hint=4;
        float distance=length(delta);
        if(distance<=EPS*4||dot(-delta/distance,FlashlightDirection.xyz)<FlashlightDirection.w)return s;
        power=FlashlightOrigin.w/(2*PI*(1-FlashlightDirection.w));
    } else if(candidate<3) {
        if(Lighting.x<=0)return s;
        uint i=candidate-1;
        float3 position=i==0?float3(-3,4,-3):float3(4,3,3);
        if(CameraState.w)position=position*float3(4,1,4)+float3(0,8,0);
        delta=position-h.p;s.hint=i;
        power=(Lighting.w==2?float3(80,80,80):(i==0?float3(25,4,38):float3(3,25,32))*(Play.x?8:1))/(4*PI);
    } else if(candidate==3) {
        if(!Play.x)return s;
        float t=dot(h.p-LightOrigin.xyz,LightDirection.xyz);
        float3 launch=h.p-LightDirection.xyz*t-LightOrigin.xyz;
        if(t<=EPS*4||abs(dot(launch,LightRight.xyz))>=LightRight.w||abs(dot(launch,LightUp.xyz))>=LightUp.w)return s;
        s.direction=-LightDirection.xyz;s.distance=t;
        s.irradiance=Optics.w/(4*LightRight.w*LightUp.w)*max(dot(n,s.direction),0)*exp(-Medium.x*t);
        return s;
    } else if(candidate==4) {
        if(!Play.x||!Water.x)return s;
        Source flood=source(3);float distance=flood.o.y-h.p.y;
        if(distance<=EPS*4||any(abs(h.p.xz-flood.o.xz)>=flood.halfSize))return s;
        s.direction=-flood.d;s.distance=distance;s.hint=3;
        s.irradiance=flood.power/(4*flood.halfSize.x*flood.halfSize.y)*max(dot(n,s.direction),0)*exp(-Medium.x*distance);
        return s;
    } else {
        if(!Play.x||Lighting.y<=0)return s;
        uint object=3+(candidate-5)%2;
        if(object==h.object)return s;
        Object obj=Objects[object];
        float3 local=mul(h.p-obj.world[3].xyz,transpose((float3x3)obj.world));
        float3 areaWeight=max(abs(local)-.38,0);
        float sum=areaWeight.x+areaWeight.y+areaWeight.z;
        if(sum<1e-6)return s;
        uint rng=hash(seed^(candidate*0x9e3779b9u));
        float choice=random(rng)*sum;
        uint axis=choice<areaWeight.x?0:(choice<areaWeight.x+areaWeight.y?1:2);
        float3 ln=0;ln[axis]=local[axis]>0?1:-1;
        float3 localSample=0;localSample[axis]=ln[axis]*.38;
        localSample[(axis+1)%3]=(random(rng)*2-1)*.38;
        localSample[(axis+2)%3]=(random(rng)*2-1)*.38;
        delta=mul(float4(localSample,1),obj.world).xyz-h.p;
        float distance=length(delta);
        if(distance<=EPS*4)return s;
        float cosineLight=max(dot(mul(ln,(float3x3)obj.world),-delta/distance),0);
        float pdf=areaWeight[axis]/(sum*.76*.76);
        power=(object==3?float3(5,.12,2.8):float3(.08,3.8,5))*cosineLight/(pdf*areaSamples);
        sigma=Medium.xxx;s.hint=object;
    }
    s.distance=length(delta);
    if(s.distance<=EPS*4)return (DiffuseLightSample)0;
    s.direction=delta/s.distance;
    float r2=s.distance*s.distance;
    scale=max(dot(n,s.direction),0)/(candidate==0?max(r2,1e-5):r2);
    s.irradiance=power*scale*exp(-sigma*s.distance);
    return s;
}
float diffuseLightWeight(float3 irradiance,float3 reflectance) {
    // Max RGB protects saturated sources/materials that luminance would starve.
    float3 contribution=irradiance*reflectance;
    return max(contribution.x,max(contribution.y,contribution.z));
}
// Pair like source types: retain strong contributions, share a bounded random
// offset, and keep only two candidate states live across visibility traversal.
float3 diffuseLightPair(Hit h,float3 n,float3 reflectance,DiffuseLightSample a,DiffuseLightSample b,
                        uint reference,inout float offset) {
    float wa=diffuseLightWeight(a.irradiance,reflectance),wb=diffuseLightWeight(b.irradiance,reflectance);
    uint active=(wa>0?1:0)+(wb>0?1:0);float total=wa+wb;
    float pa=lambertianProbability(wa,total,active,1.5,reference);
    float pb=lambertianProbability(wb,total,active,1.5,reference);
    bool selectA=lambertianSelected(pa,offset);offset=lambertianNextOffset(pa,offset);
    bool selectB=lambertianSelected(pb,offset);offset=lambertianNextOffset(pb,offset);
    float3 result=0;
    if(selectA&&!occluded(h.p+n*EPS*2,a.direction,255,a.hint,a.distance-EPS*4))result+=a.irradiance/pa;
    if(selectB&&!occluded(h.p+n*EPS*2,b.direction,255,b.hint,b.distance-EPS*4))result+=b.irradiance/pb;
    return result;
}
float3 directLight(Hit h,float3 n,inout uint rng,uint areaSamples=1,float3 reflectance=1) {
    float3 result=laserIrradiance(h,n);
    // Higher optical budgets request full visibility as well as more area
    // samples. Parent RNG consumption is fixed in all modes, including replay.
    // Measured triangle-only visibility is too cheap to amortize thinning.
    // Enable it for the expensive procedural-water workload, not just because
    // a source happens to be dim.
    uint reference=(OpticalControls.w&1024u)||(areaSamples>1)||!FluidState.x;
    rng=hash(rng);uint seed=rng;
    uint selectionRng=hash(seed^0xd1b54a35u);float offset=random(selectionRng);
    float3 sigma=extinctionRgb(ambientMedium(h.p));
    result+=diffuseLightPair(h,n,reflectance,
        diffuseLightSample(h,n,sigma,1,seed,areaSamples),diffuseLightSample(h,n,sigma,2,seed,areaSamples),reference,offset);
    // Collimated direct complements and the camera flashlight stay exact. In
    // particular, moving water shadows under the room flood gain no extra noise.
    [unroll]for(uint i=0;i<3;++i) {
        uint candidate=i==0?0:i+2;
        DiffuseLightSample s=diffuseLightSample(h,n,sigma,candidate,seed,areaSamples);
        if(diffuseLightWeight(s.irradiance,reflectance)>0&&
           !occluded(h.p+n*EPS*2,s.direction,255,s.hint,s.distance-EPS*4))result+=s.irradiance;
    }
    if(Play.x&&Lighting.y>0)for(uint i=0;i<areaSamples;++i)
        result+=diffuseLightPair(h,n,reflectance,
            diffuseLightSample(h,n,sigma,5+2*i,seed,areaSamples),
            diffuseLightSample(h,n,sigma,6+2*i,seed,areaSamples),reference,offset);
    return result;
}
