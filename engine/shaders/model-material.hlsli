#ifndef LAB_MODEL_MATERIAL
#define LAB_MODEL_MATERIAL
static const uint MODEL_MATERIAL_BASE=64;
struct ModelAttributes { float4 normal,tangent,uv,color; };
struct ModelTexture { uint image,uvSet,channel,samplerIndex; float4 u,v; };
struct ModelMaterial {
    float4 baseColor,emissive,factors;
    float alphaCutoff; uint alphaMode,doubleSided,unlit;
    ModelTexture textures[7];
};
StructuredBuffer<ModelMaterial> ModelMaterials : register(t10);
StructuredBuffer<ModelAttributes> ModelVertices : register(t11);
Texture2D<float4> ModelImages[512] : register(t0,space2);
SamplerState ModelSamplers[18] : register(s0,space2);
float2 modelUv(ModelTexture t,float4 uv) {
    float3 p=float3(t.uvSet?uv.zw:uv.xy,1);
    return float2(dot(t.u.xyz,p),dot(t.v.xyz,p));
}
float4 modelTexture(ModelTexture t,float4 uv,float4 footprint=0) {
    if(t.image==0xffffffff)return 1;
    uint image=NonUniformResourceIndex(t.image),w,h,levels;
    ModelImages[image].GetDimensions(0,w,h,levels);
    float2 size=t.uvSet?footprint.zw:footprint.xy;
    float texels=max(w*dot(abs(t.u.xy),size),h*dot(abs(t.v.xy),size));
    float lod=max(0,log2(max(1e-8,texels)));
    float2 p=modelUv(t,uv);
    return ModelImages[image].SampleLevel(ModelSamplers[NonUniformResourceIndex(t.samplerIndex)],p,lod);
}
struct ModelSurface { float3 base,emission; float metallic,roughness,occlusion; uint unlit; };
ModelSurface modelSurface(ModelMaterial m,float4 uv,float4 color,float4 footprint) {
    ModelSurface s;
    s.base=max(0,m.baseColor.rgb*color.rgb*modelTexture(m.textures[0],uv,footprint).rgb);
    s.emission=Lighting.y*m.emissive.rgb*modelTexture(m.textures[5],uv,footprint).rgb;
    s.metallic=saturate(m.factors.x*modelTexture(m.textures[1],uv,footprint)[m.textures[1].channel]);
    s.roughness=clamp(m.factors.y*modelTexture(m.textures[2],uv,footprint)[m.textures[2].channel],.045,1);
    s.occlusion=lerp(1,modelTexture(m.textures[4],uv,footprint).r,m.factors.w);
    s.unlit=m.unlit;
    return s;
}
bool modelAcceptHit(uint object,uint primitive,float2 bary,float3 localDirection,
                    float3 origin,float3 direction,float distance) {
    uint index=Objects[object].info.x+primitive*3;
    Vertex a=Vertices[index],b=Vertices[index+1],c=Vertices[index+2];
    if(a.material<MODEL_MATERIAL_BASE)return true;
    ModelMaterial m=ModelMaterials[a.material-MODEL_MATERIAL_BASE];
    float3 geometric=cross(b.position-a.position,c.position-a.position);
    if(!m.doubleSided&&dot(geometric,localDirection)>=0)return false;
    if(m.alphaMode==0)return true;
    float3 weights=float3(1-bary.x-bary.y,bary);
    ModelAttributes va=ModelVertices[a.pad],vb=ModelVertices[b.pad],vc=ModelVertices[c.pad];
    float4 uv=va.uv*weights.x+vb.uv*weights.y+vc.uv*weights.z;
    float alpha=saturate(m.baseColor.a*(va.color.a*weights.x+vb.color.a*weights.y+vc.color.a*weights.z)*modelTexture(m.textures[0],uv).a);
    alpha*=modelTexture(m.textures[6],uv).r;
    if(m.alphaMode==1)return alpha>=m.alphaCutoff;
    // Stochastic coverage works for primary, indirect, photon and shadow rays.
    uint rng=hash(asuint(origin.x)^asuint(origin.y)^asuint(direction.z)^asuint(distance)^
        primitive^(object*0x9e3779b9u)^(Dimensions.z*0x517cc1b7u));
    return random(rng)<alpha;
}
[shader("anyhit")]
void ModelAnyHit(inout Payload payload,BuiltInTriangleIntersectionAttributes bary) {
    if(!modelAcceptHit(InstanceID(),PrimitiveIndex(),bary.barycentrics,ObjectRayDirection(),
                       WorldRayOrigin(),WorldRayDirection(),RayTCurrent()))IgnoreHit();
}
#endif
