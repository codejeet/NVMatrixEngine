// Imported surfaces use their full GGX/diffuse BSDF. Existing ReSTIR reservoirs
// remain diffuse; imported primary hits take this path in either rendering mode.
float3 modelPath(Hit first,inout uint rng,uint samples,out float distance) {
    float3 result=0;distance=0;
    for(uint sample=0;sample<samples;++sample) {
        Hit h=first;float3 throughput=1;float emissionWeight=1;
        uint lightSamples=max(1,Sampling.y);
        uint bounces=max(1,Sampling.w);
        for(uint bounce=0;bounce<bounces;++bounce) {
            float3 normal=dot(h.n,h.view)>0?h.n:-h.n;
            result+=throughput*emission(h)*emissionWeight/samples;
            if(h.model.unlit)break;
            result+=throughput*modelDirect(h,normal,h.view,rng,lightSamples,bounce+1<bounces)/samples;
            if(bounce+1==bounces)break;
            float3 direction,weight;
            if(!modelScatter(h.model,normal,h.view,rng,direction,weight))break;
            float bsdfPdf=modelPdf(h.model,normal,h.view,direction);
            throughput*=weight;
            // Terminate low-throughput tails without systematically darkening
            // indirect lighting. Keep the first scatter for DLSS hit-distance
            // guides; surviving paths compensate for their survival chance.
            if(SamplingState.y&&bounce>=1) {
                float survival=clamp(max(throughput.x,max(throughput.y,throughput.z)),.05,.95);
                if(random(rng)>=survival)break;
                throughput/=survival;
            }
            float3 origin=h.p+normal*EPS*2;
            Payload p=trace(origin,direction,255,bounce+1);
            if(sample==0&&bounce==0)distance=p.object==0xffffffff?0:p.t;
            uint medium=ambientMedium(origin);
            float3 segmentWeight=throughput;
            result+=throughput*beamRadiance(origin,direction,p.t,medium)/samples;
            throughput*=exp(-extinctionRgb(medium)*p.t);
            if(p.object==0xffffffff) {
                if(CameraState.w!=3)result+=throughput*sky(direction)/samples;
                break;
            }
            h=surface(p,origin,direction,2*CameraUp.w/Dimensions.y*p.t);
            float areaPdf=h.emitterPdf;
            float lightPdf=areaPdf*p.t*p.t/max(1e-7,abs(dot(h.geometric,-direction)));
            emissionWeight=h.emitterPdf>0?modelMis(bsdfPdf,lightSamples*lightPdf):1;
            if(h.material<MODEL_MATERIAL_BASE) {
                if(glass(h)) {
                    // dielectricView applies this segment's attenuation itself.
                    float hitDistance;uint3 events;
                    float3 light=dielectricView(origin,direction,true,p,rng,hitDistance,events);
                    // We already accounted for the segment above.
                    light-=beamRadiance(origin,direction,p.t,medium);
                    result+=segmentWeight*light/samples;
                } else {
                    float3 n=dot(h.n,direction)<0?h.n:-h.n;
                    float3 light=endpoint(h,n,rng);
                    if(explicitlySampledEmitter(h))light-=emission(h);
                    result+=throughput*light/samples;
                }
                break;
            }
        }
    }
    return result;
}
