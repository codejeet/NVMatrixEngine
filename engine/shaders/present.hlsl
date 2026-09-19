#include "lens-mapping.hlsli"
Texture2D<float4> Color : register(t0);
Texture2D<float4> Auxiliary : register(t1); // bloom in PS, UI in FGComposite
RWTexture2D<float> FgDeviceDepth : register(u0);
cbuffer LensConstants : register(b0) { float2 OutputSize; float Fisheye; float HalfDiagonal; float Bloom; };
float3 lensColor(float2 pixel) {
    if(!Fisheye) return Color.Load(int3(clamp(pixel,0,OutputSize-1),0)).rgb;
    float2 uv=fisheyeToRectilinearUv(pixel/OutputSize,OutputSize.x/OutputSize.y,HalfDiagonal);
    float2 p=uv*OutputSize-.5,f=frac(p);int2 a=int2(floor(p));
    int2 hi=int2(OutputSize)-1;
    return lerp(lerp(Color.Load(int3(clamp(a,0,hi),0)).rgb,Color.Load(int3(clamp(a+int2(1,0),0,hi),0)).rgb,f.x),
                lerp(Color.Load(int3(clamp(a+int2(0,1),0,hi),0)).rgb,Color.Load(int3(clamp(a+1,0,hi),0)).rgb,f.x),f.y);
}
struct V { float4 position:SV_Position; };
V VS(uint id:SV_VertexID) { V v; v.position=float4(id==2?3:-1,id==1?3:-1,0,1); return v; }
float4 PS(V v):SV_Target {
    float3 c=max(lensColor(v.position.xy),0);
    if(Bloom>0) {
        uint w,h;Auxiliary.GetDimensions(w,h);
        float2 uv=Fisheye?fisheyeToRectilinearUv(v.position.xy/OutputSize,OutputSize.x/OutputSize.y,HalfDiagonal):v.position.xy/OutputSize;
        float2 q=uv*float2(w,h)-.5,f=frac(q);int2 a=int2(floor(q)),hi=int2(w,h)-1;
        float3 glow=lerp(lerp(Auxiliary.Load(int3(clamp(a,0,hi),0)).rgb,
                             Auxiliary.Load(int3(clamp(a+int2(1,0),0,hi),0)).rgb,f.x),
                         lerp(Auxiliary.Load(int3(clamp(a+int2(0,1),0,hi),0)).rgb,
                              Auxiliary.Load(int3(clamp(a+1,0,hi),0)).rgb,f.x),f.y);
        c+=Bloom*glow;
    }
    c=(c*(2.51*c+.03))/(c*(2.43*c+.59)+.14);
    return float4(pow(saturate(c),1/2.2),1);
}
float4 FGComposite(V v):SV_Target {
    float4 scene=Color.Load(int3(v.position.xy,0)),ui=Auxiliary.Load(int3(v.position.xy,0));
    return float4(ui.rgb+(1-ui.a)*scene.rgb,1);
}
[numthreads(8,8,1)]
void FGDepth(uint3 id:SV_DispatchThreadID) {
    uint w,h;FgDeviceDepth.GetDimensions(w,h);if(id.x>=w||id.y>=h)return;
    float z=max(.05,Color.Load(int3(id.xy,0)).x);
    // Same LH projection and 0.05/200 metre planes as the camera/SL constants.
    FgDeviceDepth[id.xy]=saturate((200.0-200.0*.05/z)/(200.0-.05));
}
