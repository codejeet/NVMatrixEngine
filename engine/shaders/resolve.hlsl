#include "common.hlsli"
[numthreads(8,8,1)]
void Clear(uint3 p:SV_DispatchThreadID) {
    if(p.x>=uint(Atlas.x)||p.y>=uint(Atlas.y)) return;
    PhotonSum.Store3((p.y*uint(Atlas.x)+p.x)*12,0);
    if(FluidState.x)FluidPhotonSum.Store3((p.y*uint(Atlas.x)+p.x)*12,0);
    // 32 diagnostics plus three GPU-only active laser segment masks.
    if(p.x<=42&&p.y==0) Stats.Store(p.x*4,0); // transport, beams, whitewater and PT diagnostics
    if((OpticalControls.x&1)&&p.x<16&&p.y==0)OpticalCounters.Store(p.x*4,0);
    uint brick=p.y*uint(Atlas.x)+p.x;
    if((OpticalControls.x&8)&&brick<FluidBricks.w)OpticalFeedback[brick]=0;
}
[numthreads(8,8,1)]
void Accumulate(uint3 p:SV_DispatchThreadID) {
    if(p.x>=uint(Atlas.x)||p.y>=uint(Atlas.y)) return;
    uint3 bits=PhotonSum.Load3((p.y*uint(Atlas.x)+p.x)*12);
    float3 power=Controls.y?asfloat(bits):float3(bits)/Atlas.w;
    float texelArea=chartArea(p.x/uint(Atlas.z))/(Atlas.z*Atlas.z);
    float receiverPower=power.y;
    if(FluidState.x) {
        uint3 fluidBits=FluidPhotonSum.Load3((p.y*uint(Atlas.x)+p.x)*12);
        float3 irradiance=(Controls.y?asfloat(fluidBits):float3(fluidBits)/Atlas.w)/texelArea;
        receiverPower+=irradiance.y*texelArea;
        // Water transport is already animated and uses a short bounded EMA.
        // Treat rigid-body/flashlight motion like fluid motion, not a whole-atlas
        // teleport. Source energy/spectrum changes and explicit resets clear it.
        if((FluidState.w&128)||FluidState.z)FluidCaustics[p.xy]=float4(irradiance,1);
        else {
            float4 history=FluidCaustics[p.xy];
            uint age=min(uint(history.w),(liquidSurfaceMoving(FluidState.y)||Controls.x)?3u:Controls.w-1);
            FluidCaustics[p.xy]=float4(lerp(history.xyz,irradiance,1.0/(age+1)),age+1);
        }
    }
    if(OpticalControls.x&1){
        uint index=p.y*uint(Atlas.x)+p.x;float4 old=0;
        if(OpticalControls.y&&!Controls.x)old=OpticalReceivers[index];
        float value=opticalCompress(receiverPower/(texelArea*OpticalParameters.w));
        OpticalReceivers[index]=float4(max(value,old.x*exp(-max(OpticalParameters.x,0)/.2)),abs(value-old.z),value,1);
    }
    if(Controls.x) { Caustics[p.xy]=float4(power/texelArea,1); return; }
    float4 old=Caustics[p.xy];
    uint age=min(uint(old.w),Controls.w-1);
    float alpha=1.0/(age+1);
    // Running average during warmup, then bounded EMA. Reset reads no old value.
    Caustics[p.xy]=float4(lerp(old.xyz,power/texelArea,alpha),age+1);
}
[numthreads(8,8,1)]
void Composite(uint3 p:SV_DispatchThreadID) {
    if(any(p.xy>=Dimensions.xy)) return;
    float4 s=Surface[p.xy];
    if(s.w>0) {
        int2 lo=int2(s.z*Atlas.z,0),hi=lo+int2(Atlas.z-1,Atlas.y-1);
        int2 q=int2(floor(s.xy)); float2 f=frac(s.xy);
        float3 a=lerp(causticTexel(clamp(q,lo,hi)),causticTexel(clamp(q+int2(1,0),lo,hi)),f.x);
        float3 b=lerp(causticTexel(clamp(q+int2(0,1),lo,hi)),causticTexel(clamp(q+1,lo,hi)),f.x);
        // Atlas stores irradiance. Lambertian radiance needs albedo / pi.
        // Keep spectral XYZ signed after RGB conversion until combined at presentation.
        float3 transmittance=exp(-extinctionRgb(ambientMedium(CameraPosition.xyz))*s.w);
        float areaScale=CameraState.w==3?max(0,NormalRoughness[p.xy].y):1;
        Noisy[p.xy]+=float4(xyzToRgb(lerp(a,b,f.y))*Albedo[p.xy].xyz*transmittance*(areaScale/PI),0);
        if(OpticalControls.x&1){
            uint index=opticalAddress(p.xy,false);
            float value=opticalReceiver((s.xy-float2(s.z*Atlas.z,0)+.5)/Atlas.z,uint(s.z));
            OpticalPixels[index].signals.z=max(OpticalPixels[index].signals.z,value);
        }
    }
    Noisy[p.xy]=float4(max(Noisy[p.xy].xyz,0),1);
}
