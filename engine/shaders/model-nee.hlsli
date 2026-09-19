// Resample unoccluded direct-light estimates before tracing visibility. Imported
// emitters, moving blocks and the flashlight share the same shadow-ray budget.
// Each imported stratum is divided by its candidate count; each separately
// sampled block/delta light already estimates its complete integral.
struct ModelDirectReservoir {
    float3 value,direction;
    float distance,pdf,selectedWeight,weightSum;
    uint hint;
};
void modelDirectCandidate(inout ModelDirectReservoir r,float3 value,float3 direction,
                          float distance,uint hint,float pdf,float bsdfImportance,inout uint rng) {
    float weight=max(value.x,max(value.y,value.z))*bsdfImportance;
    if(weight<=0)return;
    r.weightSum+=weight;
    if(random(rng)*r.weightSum<weight) {
        r.value=value;r.direction=direction;r.distance=distance;
        r.selectedWeight=weight;r.hint=hint;r.pdf=pdf;
    }
}
float3 modelResampledDirect(Hit h,float3 n,float3 view,inout uint rng,uint samples,bool mis,bool pbr=true) {
    float3 result=0;
    uint candidates=max(1,Sampling.z);
    float3 sigma=extinctionRgb(ambientMedium(h.p));
    for(uint sample=0;sample<samples;++sample) {
        ModelDirectReservoir r=(ModelDirectReservoir)0;
        for(uint i=0;i<candidates;++i) {
            ModelLightSample light=modelEmitterSample(h.p,rng);
            float cosine=saturate(dot(n,light.direction));
            if(light.pdf<=0||cosine<=0||!any(light.radiance>0))continue;
            float3 value=light.radiance*(cosine/(light.pdf*candidates))*exp(-sigma*light.distance);
            // PDF-based importance is cheaper than evaluating the complete
            // colored BRDF for every candidate. Evaluate it only if selected
            // and visible; the reservoir compensation retains the estimator.
            float importance=pbr?modelPdf(h.model,n,view,light.direction)/max(1e-7,cosine):1;
            modelDirectCandidate(r,value,light.direction,light.distance,0,light.pdf,importance,rng);
        }
        uint seed=rng;random(rng);
        // The flashlight and moving area emitters retain the fast sampler
        // strata. Other scene lights join the same visibility reservoir.
        for(uint i=0;i<(CameraState.w==3?1u:3u);++i) {
            uint candidate=i==0?0:i+4;
            DiffuseLightSample light=diffuseLightSample(h,n,sigma,candidate,seed,1);
            float cosine=saturate(dot(n,light.direction));
            float importance=pbr?modelPdf(h.model,n,view,light.direction)/max(1e-7,cosine):1;
            modelDirectCandidate(r,light.irradiance,light.direction,light.distance,light.hint,0,importance,rng);
        }
        if(CameraState.w!=3) {
            // Skip inactive classes before generating candidates. In Neon all
            // four are absent, preserving the original candidate workload.
            for(uint type=1;type<5;++type) {
                bool enabled=type<3?Lighting.x>0:type==3?(Play.x&&Optics.w>0):(Play.x&&Water.x&&Medium.w>0);
                if(!enabled)continue;
                DiffuseLightSample light=diffuseLightSample(h,n,sigma,type,seed,1);
                float cosine=saturate(dot(n,light.direction));
                float importance=pbr?modelPdf(h.model,n,view,light.direction)/max(1e-7,cosine):1;
                modelDirectCandidate(r,light.irradiance,light.direction,light.distance,light.hint,0,importance,rng);
            }
        } else {
            float3 direction;float pdf;
            float3 radiance=oceanEnvironmentSample(rng,direction,pdf);
            float cosine=saturate(dot(n,direction));
            float importance=pbr?modelPdf(h.model,n,view,direction)/max(1e-7,cosine):1;
            if(pdf>0)modelDirectCandidate(r,radiance*(cosine/pdf),direction,10000,0,0,importance,rng);
            if(oceanLayer())for(uint lamp=0;lamp<OCEAN_LANTERN_COUNT;++lamp) {
                float3 delta=oceanLanternPosition(lamp)-h.p;
                float d2=dot(delta,delta),r2=OCEAN_LANTERN_RADIUS*OCEAN_LANTERN_RADIUS;
                if(d2<=r2)continue;
                float3 axis=normalize(delta),x,y;basis(axis,x,y);
                float omc=(r2/d2)/(1+sqrt(max(0,1-r2/d2)));
                float c=1-random(rng)*omc,phi=2*PI*random(rng);
                direction=axis*c+sqrt(max(0,1-c*c))*(x*cos(phi)+y*sin(phi));
                float projection=dot(delta,direction);
                float distance=projection-sqrt(max(0,r2-d2+projection*projection));
                cosine=saturate(dot(n,direction));
                importance=pbr?modelPdf(h.model,n,view,direction)/max(1e-7,cosine):1;
                float3 value=oceanLanternRadiance()*(cosine*2*PI*omc)*exp(-Medium.x*distance);
                modelDirectCandidate(r,value,direction,distance,0,0,importance,rng);
            }
        }
        if(r.selectedWeight>0&&
           !occluded(h.p+n*EPS*2,r.direction,255,r.hint,r.distance-EPS*4)) {
            // Use the original proposal for complementary light/BSDF MIS.
            // The random reservoir weight is not the density of a BSDF hit.
            float weight=mis&&r.pdf>0?modelMis(r.pdf*samples,modelPdf(h.model,n,view,r.direction)):1;
            result+=r.value*(pbr?modelBrdf(h.model,n,view,r.direction):float3(1,1,1))*
                (weight*r.weightSum/(r.selectedWeight*samples));
        }
    }
    return result;
}
