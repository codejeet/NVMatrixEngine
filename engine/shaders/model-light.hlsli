float3 modelF0(ModelSurface s) { return lerp(.04,s.base,s.metallic); }
float modelG1(float cosine,float alpha2) {
    return 2*cosine/max(1e-7,cosine+sqrt(alpha2+(1-alpha2)*cosine*cosine));
}
float3 modelBrdf(ModelSurface s,float3 n,float3 v,float3 l,bool indirect=false) {
    float nv=saturate(dot(n,v)),nl=saturate(dot(n,l));
    float3 sum=v+l;if(nv<=0||nl<=0||dot(sum,sum)<1e-12)return 0;
    float3 h=normalize(sum);
    float nh=saturate(dot(n,h)),vh=saturate(dot(v,h));
    float alpha=s.roughness*s.roughness,a2=alpha*alpha;
    float denominator=nh*nh*(a2-1)+1;
    float distribution=a2/(PI*denominator*denominator);
    float3 fresnel=modelF0(s)+(1-modelF0(s))*pow(1-vh,5);
    float3 specular=fresnel*(distribution*modelG1(nv,a2)*modelG1(nl,a2)/max(1e-7,4*nv*nl));
    float3 diffuse=(1-fresnel)*(1-s.metallic)*s.base/PI;
    return specular+diffuse*(indirect?s.occlusion:1);
}
float modelPdf(ModelSurface s,float3 n,float3 view,float3 direction) {
    float nl=dot(n,direction);
    if(nl<=0||dot(view+direction,view+direction)<1e-12)return 0;
    float3 halfVector=normalize(view+direction);
    float nh=saturate(dot(n,halfVector)),vh=saturate(dot(view,halfVector));
    float a2=pow(s.roughness,4),den=nh*nh*(a2-1)+1;
    float specPdf=a2*nh/max(1e-12,4*PI*den*den*vh);
    return lerp(.5,1,s.metallic)*specPdf+(1-lerp(.5,1,s.metallic))*nl/PI;
}
#include "model-nee.hlsli"
float3 modelDirect(Hit h,float3 n,float3 view,inout uint rng,uint samples=1,bool mis=false) {
    if(SamplingState.x)return modelResampledDirect(h,n,view,rng,samples,mis);
    float3 result=0;
    uint seed=rng;random(rng);
    for(uint i=0;i<samples;++i) {
        ModelLightSample light=modelEmitterSample(h.p,rng);
        float cosine=saturate(dot(n,light.direction));
        if(light.pdf>0&&cosine>0&&any(light.radiance>0)&&
           !occluded(h.p+n*EPS*2,light.direction,255,0,light.distance-EPS*4)) {
            float weight=mis?modelMis(light.pdf*samples,modelPdf(h.model,n,view,light.direction)):1;
            result+=light.radiance*modelBrdf(h.model,n,view,light.direction)*(cosine*weight/(light.pdf*samples))*
                exp(-extinctionRgb(ambientMedium(h.p))*light.distance);
        }
    }
    if(CameraState.w!=3) {
        uint cubeSamples=CameraState.w==4?1:samples;
        uint count=5+((Play.x||CameraState.w==4)?2*cubeSamples:0);
        for(uint i=0;i<count;++i) {
            DiffuseLightSample light=diffuseLightSample(h,n,extinctionRgb(ambientMedium(h.p)),i,seed,cubeSamples);
            if(any(light.irradiance>0)&&!occluded(h.p+n*EPS*2,light.direction,255,light.hint,light.distance-EPS*4))
                result+=light.irradiance*modelBrdf(h.model,n,view,light.direction);
        }
    } else {
        for(uint i=0;i<samples;++i) {
            float3 direction;float pdf;
            float3 radiance=oceanEnvironmentSample(rng,direction,pdf);
            float cosine=saturate(dot(n,direction));
            if(pdf>0&&cosine>0&&!occluded(h.p+n*EPS*2,direction,255,0,10000))
                result+=radiance*modelBrdf(h.model,n,view,direction)*(cosine/(pdf*samples));
        }
        if(oceanLayer())for(uint lamp=0;lamp<OCEAN_LANTERN_COUNT;++lamp) {
            float3 delta=oceanLanternPosition(lamp)-h.p;
            float d2=dot(delta,delta),r2=OCEAN_LANTERN_RADIUS*OCEAN_LANTERN_RADIUS;
            if(d2<=r2)continue;
            float3 axis=normalize(delta),x,y;basis(axis,x,y);
            float oneMinusCos=(r2/d2)/(1+sqrt(max(0,1-r2/d2)));
            float cosine=1-random(rng)*oneMinusCos,phi=2*PI*random(rng);
            float3 direction=axis*cosine+sqrt(max(0,1-cosine*cosine))*(x*cos(phi)+y*sin(phi));
            float projection=dot(delta,direction),distance=projection-sqrt(max(0,r2-d2+projection*projection));
            if(dot(n,direction)>0&&!occluded(h.p+n*EPS*2,direction,255,0,distance-EPS*4))
                result+=oceanLanternRadiance()*modelBrdf(h.model,n,view,direction)*
                    (saturate(dot(n,direction))*2*PI*oneMinusCos)*exp(-Medium.x*distance);
        }
        DiffuseLightSample torch=diffuseLightSample(h,n,extinctionRgb(ambientMedium(h.p)),0,seed,1);
        if(any(torch.irradiance>0)&&!occluded(h.p+n*EPS*2,torch.direction,255,4,torch.distance-EPS*4))
            result+=torch.irradiance*modelBrdf(h.model,n,view,torch.direction);
    }
    return result;
}
bool modelScatter(ModelSurface s,float3 n,float3 view,inout uint rng,out float3 direction,out float3 weight) {
    float probability=lerp(.5,1,s.metallic);
    if(random(rng)<probability) {
        float alpha=s.roughness*s.roughness,u=random(rng),phi=2*PI*random(rng);
        float cosine=sqrt((1-u)/(1+(alpha*alpha-1)*u));
        float3 x,y;basis(n,x,y);
        float3 halfVector=n*cosine+sqrt(max(0,1-cosine*cosine))*(x*cos(phi)+y*sin(phi));
        direction=reflect(-view,halfVector);
    } else direction=diffuseDirection(n,rng);
    float nl=dot(n,direction);weight=0;
    if(nl<=0||dot(view+direction,view+direction)<1e-12)return false;
    float pdf=modelPdf(s,n,view,direction);
    if(pdf<=0)return false;
    weight=modelBrdf(s,n,view,direction,true)*nl/pdf;
    return all(isfinite(weight));
}
