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
        if(CameraState.w==2)position.y+=1.16;
        if(CameraState.w==1)position=position*float3(4,1,4)+float3(0,8,0);
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
        if((!Play.x&&CameraState.w!=4)||Lighting.y<=0)return s;
        uint object=(CameraState.w==4?2:3)+(candidate-5)%2;
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
        power=((candidate-5)%2==0?float3(5,.12,2.8):float3(.08,3.8,5))*cosineLight/(pdf*areaSamples);
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
float3 oceanSlabIrradiance(Hit h) {
    float3 irradiance=0;
        if(h.material==18&&h.chart==0xffffffff) {
            // Outside the simulation the ocean is a flat optical slab. Its
            // refracted sunlight has an analytic solution; the inner domain
            // retains photon caustics. This avoids ending illumination at the
            // rectangular photon atlas and does not add interactive fluid.
            float3 sun=OceanEnvironment[1].xyz;
            if(Lighting.w<5&&sun.y>0) {
                float3 direction=-refract(-sun,float3(0,1,0),1/mediumIndex(8,550));
                float distance=(6.02-h.p.y)/max(.01,direction.y);
                float3 entry=h.p+direction*distance;
                if(!occluded(entry+sun*EPS*4,sun,255,0,10000)) {
                    float3 xyz=0;
                    for(uint band=0;band<32;++band) {
                        float nm=380+(band+.5)*(400.0/32);
                        float ni=mediumIndex(8,nm);
                        float cosine=sqrt(1-(1-sun.y*sun.y)/(ni*ni));
                        float attenuation=exp(-extinction(8,nm)*(6.02-h.p.y)/cosine);
                        xyz+=spectralPower(nm,OceanEnvironment[1].w/32)*(1-fresnel(sun.y,1,ni))*attenuation;
                    }
                    irradiance=max(0,xyzToRgb(xyz));
                }
            }
            return irradiance;
        }
    return irradiance;
}
float3 referenceDirectLight(Hit h,float3 n,inout uint rng,uint areaSamples=1,float3 reflectance=1) {
    if(CameraState.w==3) {
        float3 irradiance=modelEmitterIrradiance(h,n,rng,areaSamples);
        irradiance+=oceanSlabIrradiance(h);
        uint samples=max(2,areaSamples);
        for(uint i=0;i<samples;++i) {
            float3 direction;float pdf;
            float3 radiance=oceanEnvironmentSample(rng,direction,pdf);
            float cosine=max(0,dot(n,direction));
            if(pdf>0&&cosine>0&&!occluded(h.p+n*EPS*2,direction,255,0,10000))
                irradiance+=radiance*(cosine/(pdf*samples));
        }
        if(oceanLayer())for(uint lamp=0;lamp<OCEAN_LANTERN_COUNT;++lamp) {
            if(h.material==24+lamp)continue;
            float3 origin=h.p+n*EPS*2,delta=oceanLanternPosition(lamp)-origin;
            float d2=dot(delta,delta),r2=OCEAN_LANTERN_RADIUS*OCEAN_LANTERN_RADIUS;
            if(d2<=r2)continue;
            float3 axis=delta*rsqrt(d2),x,y;basis(axis,x,y);
            float cosMax=sqrt(max(0,1-r2/d2));
            // Stable 1-cos(theta), including distant lamps. Sample their visible
            // solid angle for area-light falloff and soft, ray-traced shadows.
            float oneMinusCos=(r2/d2)/(1+cosMax);
            float cosTheta=1-random(rng)*oneMinusCos;
            float sinTheta=sqrt(max(0,1-cosTheta*cosTheta)),phi=2*PI*random(rng);
            float3 direction=axis*cosTheta+sinTheta*(x*cos(phi)+y*sin(phi));
            float projection=dot(delta,direction);
            float distance=projection-sqrt(max(0,r2-d2+projection*projection));
            float cosine=max(0,dot(n,direction));
            if(cosine>0&&!occluded(origin,direction,255,0,distance-EPS*4))
                irradiance+=oceanLanternRadiance()*(cosine*2*PI*oneMinusCos)*exp(-Medium.x*distance);
        }
        DiffuseLightSample torch=diffuseLightSample(h,n,extinctionRgb(ambientMedium(h.p)),0,rng,1);
        if(any(torch.irradiance>0)&&!occluded(h.p+n*EPS*2,torch.direction,255,4,torch.distance-EPS*4))
            irradiance+=torch.irradiance;
        return irradiance;
    }
    float3 result=laserIrradiance(h,n)+modelEmitterIrradiance(h,n,rng,areaSamples);
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
    if((Play.x||CameraState.w==4)&&Lighting.y>0)for(uint i=0;i<areaSamples;++i)
        result+=diffuseLightPair(h,n,reflectance,
            diffuseLightSample(h,n,sigma,5+2*i,seed,areaSamples),
            diffuseLightSample(h,n,sigma,6+2*i,seed,areaSamples),reference,offset);
    return result;
}
