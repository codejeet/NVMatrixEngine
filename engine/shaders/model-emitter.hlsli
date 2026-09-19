// Static imported emissive triangles. Attribute normal.w carries the area proposal PDF.
#include "model-emitter-data.hlsli"
struct ModelLightSample { float3 direction,radiance; float distance,pdf; };
float modelMis(float a,float b) {
    float p=a/max(1e-20,a+b);
    return p*p/max(1e-20,p*p+(1-p)*(1-p));
}
ModelLightSample modelEmitterAt(float3 origin,uint low,inout uint rng) {
    ModelLightSample s=(ModelLightSample)0;
    if(!low||Lighting.y<=0)return s;
    ModelEmitter light=ModelLights[low];
    float root=sqrt(random(rng)),v=random(rng);
    float3 bary=float3(1-root,root*(1-v),root*v);
    float3 lightPosition=light.pArea.xyz+light.e1Cdf.xyz*bary.y+light.e2Pdf.xyz*bary.z;
    float3 delta=lightPosition-origin;s.distance=length(delta);
    if(s.distance<EPS*4)return s;
    s.direction=delta/s.distance;
    float3 normal=normalize(cross(light.e1Cdf.xyz,light.e2Pdf.xyz));
    ModelMaterial m=ModelMaterials[light.material];
    float cosine=dot(normal,-s.direction);if(m.doubleSided)cosine=abs(cosine);
    if(cosine<=1e-7)return s;
    s.pdf=light.e2Pdf.w*s.distance*s.distance/cosine;
    float4 uv=ModelVertices[light.a].uv*bary.x+ModelVertices[light.b].uv*bary.y+ModelVertices[light.c].uv*bary.z;
    float alpha=1;
    if(m.alphaMode) {
        alpha=saturate(m.baseColor.a*modelTexture(m.textures[0],uv).a*modelTexture(m.textures[6],uv).r*
            (ModelVertices[light.a].color.a*bary.x+ModelVertices[light.b].color.a*bary.y+ModelVertices[light.c].color.a*bary.z));
        if(m.alphaMode==1)alpha=alpha>=m.alphaCutoff?1:0;
    }
    s.radiance=Lighting.y*m.emissive.rgb*modelTexture(m.textures[5],uv).rgb*alpha;
    return s;
}
ModelLightSample modelEmitterSample(float3 origin,inout uint rng) {
    uint count=uint(ModelLights[0].pArea.x);
    if(!count||Lighting.y<=0)return (ModelLightSample)0;
    float choice=random(rng);uint low=1,high=count;
    while(low<high) {uint mid=(low+high)/2;if(choice<ModelLights[mid].e1Cdf.w)high=mid;else low=mid+1;}
    return modelEmitterAt(origin,low,rng);
}
float3 modelEmitterIrradiance(Hit h,float3 n,inout uint rng,uint samples) {
    float3 result=0;
    for(uint i=0;i<samples;++i) {
        ModelLightSample s=modelEmitterSample(h.p,rng);
        float cosine=saturate(dot(n,s.direction));
        if(s.pdf>0&&cosine>0&&any(s.radiance>0)&&
           !occluded(h.p+n*EPS*2,s.direction,255,0,s.distance-EPS*4))
            result+=s.radiance*(cosine/(s.pdf*samples))*exp(-extinctionRgb(ambientMedium(h.p))*s.distance);
    }
    return result;
}
