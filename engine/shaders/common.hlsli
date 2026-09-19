#ifndef LAB_COMMON
#define LAB_COMMON
static const float PI = 3.14159265359;
static const float EPS = 0.0002;
struct Vertex { float3 position; uint material; float2 uv; uint chart; uint pad; };
struct Object { row_major float4x4 world, previous; uint4 info; };
cbuffer Frame : register(b0) {
    float4 CameraPosition, CameraRight, CameraUp, CameraForward;
    row_major float4x4 ViewProjection, PreviousViewProjection;
    uint4 Dimensions, Controls;
    float4 Jitter, LightOrigin, LightDirection, LightRight, LightUp, Optics, Atlas;
    float4 Play, Sensor, Water, Medium;
    float4 FluidMinimumSpacing;
    uint4 FluidBricks,FluidState;
    uint4 PTControls; // enabled, temporal-valid, history cap, spatial neighbors
    float4 PTPreviousCamera;
    uint4 OpticalControls; // flags; history valid; max samples; debug/reference bits (256/512/1024/2048)
    float4 OpticalParameters; // dt, reprojection tolerance, variance scale, receiver irradiance scale
    float4 Lighting, FlashlightOrigin, FlashlightDirection;
    uint4 CameraState;
    uint4 Sampling; // base camera paths, full light samples, candidates, surface bounces
    uint4 SamplingState; // mode (reference/RIS), roulette, reserved, reserved
};
#include "fluid/field.hlsli"
RaytracingAccelerationStructure Scene : register(t0);
StructuredBuffer<Vertex> Vertices : register(t1);
StructuredBuffer<Object> Objects : register(t2);
StructuredBuffer<float4> Cie : register(t3);
RWTexture2D<float4> Noisy : register(u0);
RWTexture2D<float2> Motion : register(u1);
RWTexture2D<float> Depth : register(u2);
RWTexture2D<float4> NormalRoughness : register(u3);
RWTexture2D<float4> Albedo : register(u4);
RWTexture2D<float4> SpecularAlbedo : register(u5);
RWTexture2D<float> SpecularDistance : register(u6);
RWByteAddressBuffer PhotonSum : register(u7); // positive XYZ, 3 fp32 or uint lanes per texel
RWTexture2D<float4> Caustics : register(u8); // texture-space irradiance XYZ, alpha age
RWTexture2D<float4> Surface : register(u9);  // atlas texel xy, chart, receiver flag
RWByteAddressBuffer Stats : register(u10);
// The first 128 bytes are readback diagnostics. GPU-only masks use spare bytes
// in this existing 256-byte UAV, reset by Clear and assembled by BeamRaygen.
static const uint BEAM_AIR_MASK=128,BEAM_WATER_MASK=132,BEAM_ENDPOINT_MASK=136;
// Two lasers, at most 16 deterministic Fresnel-split segments each. Power is
// exposure-scaled photometric XYZ; segment integration is single scattering.
struct Beam { float4 originRadius, directionLength, powerSigma, medium; };
RWStructuredBuffer<Beam> Beams : register(u11);
RWByteAddressBuffer FluidPhotonSum : register(u12);
RWTexture2D<float4> FluidCaustics : register(u13);
float3 causticTexel(int2 p){return Caustics[p].xyz+(FluidState.x?FluidCaustics[p].xyz:0);}
uint hash(uint x) { x^=x>>16; x*=0x7feb352d; x^=x>>15; x*=0x846ca68b; return x^(x>>16); }
float random(inout uint s) { s=hash(s); return (s>>8)*(1.0/16777216.0); }
float3 xyzToRgb(float3 c) {
    return float3(dot(c,float3(3.2406,-1.5372,-.4986)),dot(c,float3(-.9689,1.8758,.0415)),dot(c,float3(.0557,-.2040,1.0570)));
}
float3 cie(float nm) {
    float x=clamp(nm-380,0,400); uint i=min(uint(x),399u);
    return lerp(Cie[i].xyz,Cie[i+1].xyz,x-i);
}
float ior(float nm) { float um=nm*.001; return Optics.x+Optics.y/(um*um); }
// HDR unit = 182.458... cd/m^2: an explicit, fixed exposure preserving the
// lab's equal-energy white calibration. Radiant watts -> 683 * CIE XYZ -> HDR.
static const float PHOTOMETRIC_EXPOSURE = 400.0/(106.856895*683.0);
float3 spectralPower(float nm,float watts) { return cie(nm)*(683.0*PHOTOMETRIC_EXPOSURE)*watts; }
float2 waterProperties(float nm) {
    float t=clamp(nm-380,0,400);uint i=min(uint(t),399u);
    return lerp(Cie[401+i].xy,Cie[402+i].xy,t-i);
}
float waterHeight(float2 p) {
    return 1.10+(Water.y?0:.065*sin(dot(p,float2(3.7,2.9)))+.035*sin(dot(p,float2(-4.3,1.7))+.8));
}
bool inWater(float3 p) {
    if(CameraState.w==3&&max(abs(p.x),abs(p.z))>=128)return p.y<6.02;
    if(FluidState.x)return liquidSample(p).x<0;
    return Water.x&&p.x>1.75&&p.x<5.65&&p.z>-4.95&&p.z<-1.25&&p.y>-.05&&p.y<waterHeight(p.xz);
}
bool inPitGlass(float3 p) {
    // Fluid lab only. This is the same connected annulus as scene.cpp, including
    // its corners, and permits correct ray origins inside a wall.
    return FluidState.x && !(FluidState.w&16) && p.y>0 && p.y<1.2 && p.x>1.6 && p.x<5.8 && p.z>-5.1 && p.z<-1.1 &&
           (p.x<1.8 || p.x>5.6 || p.z<-4.9 || p.z>-1.3);
}
uint ambientMedium(float3 p) {return inPitGlass(p)?10:(inWater(p)?8:0);}
float mediumIndex(uint medium,float nm) {return medium==8?waterProperties(nm).x:((medium==0||medium==12)?1:ior(nm));}
float extinction(uint medium,float nm) {
    return medium==12?0:(medium==8?waterProperties(nm).y+Medium.y:(medium==10?.005:(medium?Optics.z:Medium.x)));
}
float3 extinctionRgb(uint medium) {
    return float3(extinction(medium,610),extinction(medium,550),extinction(medium,460));
}
float fresnel(float cosine,float ni,float nt) {
    float sin2=(ni/nt)*(ni/nt)*max(0,1-cosine*cosine);
    if(sin2>=1) return 1;
    float ct=sqrt(1-sin2);
    float rs=(ni*cosine-nt*ct)/(ni*cosine+nt*ct),rp=(nt*cosine-ni*ct)/(nt*cosine+ni*ct);
    return .5*(rs*rs+rp*rp);
}
void basis(float3 n,out float3 x,out float3 y) {
    x=normalize(cross(abs(n.y)<.95?float3(0,1,0):float3(1,0,0),n)); y=cross(n,x);
}
float3 diffuseDirection(float3 n,inout uint rng) {
    float u=random(rng),p=2*PI*random(rng); float3 x,y; basis(n,x,y);
    return x*(sqrt(u)*cos(p))+y*(sqrt(u)*sin(p))+n*sqrt(1-u);
}
float chartArea(uint chart) {
    if(CameraState.w==3)return 65536;
    if(CameraState.w==2)return chart==0?672:(chart==1?171.84:200.48);
    if(CameraState.w==1)return chart==0?2688:((chart==1||chart==4)?672:784);
    return chart==4?13.68:(chart==0?168:(chart==1?72:84));
}
float2 atlasPixel(float2 uv,uint chart) { return float2(chart*Atlas.z,0)+saturate(uv)*Atlas.z-.5; }
// Exact box integral of the periodic narrow line, not a smoothstep that changes
// its average reflectance as it becomes subpixel. Also valid at negative UVs.
float gridLineIntegral(float x,float halfLine) {
    float f=frac(x);
    return floor(x)*(2*halfLine)+min(f,halfLine)+max(f-(1-halfLine),0);
}
float gridCoverage(float2 uv,float2 footprint,float halfLine) {
    float2 w=max(footprint,1e-4),a=uv-w*.5,b=uv+w*.5;
    float2 coverage=float2(gridLineIntegral(b.x,halfLine)-gridLineIntegral(a.x,halfLine),
                           gridLineIntegral(b.y,halfLine)-gridLineIntegral(a.y,halfLine))/w;
    coverage=saturate(coverage);
    return coverage.x+coverage.y-coverage.x*coverage.y;
}
float3 receiverAlbedo(uint chart,float2 uv,float2 footprint=0) {
    if(Play.x) {
        if(CameraState.w) {
            float2 metres=CameraState.w==2?(chart==0?float2(28,24):float2(7.16,chart==1?24:28)):
                (chart==0?float2(56,48):float2(14,(chart==1||chart==4)?48:56));
            float coverage=gridCoverage(uv*metres,footprint*metres,.013);
            return (chart==0?float3(.82,.82,.82):float3(.33,.38,.43))*lerp(1,.18,coverage);
        }
        if(chart==4) {
            float coverage=gridCoverage(uv*float2(19,18),footprint*float2(19,18),.022);
            return lerp(1,.18,coverage)*float3(.82,.82,.82);
        }
        // Neutral, physically plausible white tile under the water. Dark metre
        // divisions make depth/refraction legible without emitting extra light.
        float3 a=chart==0?float3(.82,.82,.82):float3(.33,.38,.43);
        float coverage=gridCoverage(uv*float2(12,14),footprint*float2(12,14),.013);
        return a*lerp(1,chart==0?.18:.4,coverage);
    }
    float3 a=chart==2?float3(.14,.2,.28):(chart==3?float3(.3,.18,.22):float3(.55,.55,.55));
    float gridLine=min(frac(uv.x*12),frac(uv.y*12));
    return a*(gridLine<.015?.65:1);
}
float3 causticIrradiance(float2 uv,uint chart) {
    if(chart>=5)return 0;
    float2 s=atlasPixel(uv,chart);int2 lo=int2(chart*uint(Atlas.z),0),hi=lo+int2(Atlas.z-1,Atlas.y-1);
    int2 q=int2(floor(s));float2 f=frac(s);
    float3 a=lerp(causticTexel(clamp(q,lo,hi)),causticTexel(clamp(q+int2(1,0),lo,hi)),f.x);
    float3 b=lerp(causticTexel(clamp(q+int2(0,1),lo,hi)),causticTexel(clamp(q+1,lo,hi)),f.x);
    return xyzToRgb(lerp(a,b,f.y));
}
#include "optical-state.hlsli"
#endif
