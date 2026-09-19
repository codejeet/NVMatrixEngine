// Quarter-resolution bright pass followed by a separable Gaussian. Contiguous
// taps avoid the duplicated neon outlines caused by sparse full-resolution taps.
Texture2D<float4> Input : register(t0);
RWTexture2D<float4> Output : register(u0);
[numthreads(8,8,1)]
void BloomDownsample(uint3 id:SV_DispatchThreadID) {
    uint w,h;Output.GetDimensions(w,h);if(any(id.xy>=uint2(w,h)))return;
    uint iw,ih;Input.GetDimensions(iw,ih);float3 color=0;
    for(uint y=0;y<4;++y)for(uint x=0;x<4;++x)
        color+=max(Input.Load(int3(min(id.xy*4+uint2(x,y),uint2(iw,ih)-1),0)).rgb-1,0)/16;
    Output[id.xy]=float4(color,1);
}
void blur(uint2 pixel,int2 axis) {
    uint w,h;Output.GetDimensions(w,h);if(any(pixel>=uint2(w,h)))return;
    float3 result=0;float total=0;
    for(int i=-12;i<=12;++i) {
        float weight=exp(-float(i*i)/32);
        result+=Input.Load(int3(clamp(int2(pixel)+axis*i,0,int2(w,h)-1),0)).rgb*weight;
        total+=weight;
    }
    Output[pixel]=float4(result/total,1);
}
[numthreads(8,8,1)]
void BloomHorizontal(uint3 id:SV_DispatchThreadID) {blur(id.xy,int2(1,0));}
[numthreads(8,8,1)]
void BloomVertical(uint3 id:SV_DispatchThreadID) {blur(id.xy,int2(0,1));}
