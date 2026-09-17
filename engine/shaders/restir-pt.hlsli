// Engine bridge to the pinned NVIDIA RTXDI ReSTIR PT implementation. This is
// multi-bounce diffuse path resampling, NOT a radiance history/denoising filter.
// Primary direct lighting and deterministic E S* D prefixes remain separate.
// A diffuse endpoint integrates the existing NEE + photon irradiance estimators;
// it is a view-independent outgoing-radiance oracle for the SDK path sampler.
#include "Rtxdi/PT/ReSTIRPTParameters.h"
#include "Rtxdi/Utils/RandomSamplerState.hlsli"
#include "Rtxdi/Utils/BrdfRaySample.hlsli"
RWStructuredBuffer<RTXDI_PackedPTReservoir> PTReservoirs : register(u14);
struct RAB_Surface { float4 positionDepth,normalValid,albedoMaterial,viewObject; };
RWStructuredBuffer<RAB_Surface> PTSurfaces : register(u15);
StructuredBuffer<float2> PTNeighborOffsets : register(t7);
#define RTXDI_PT_RESERVOIR_BUFFER PTReservoirs
#define RTXDI_NEIGHBOR_OFFSETS_BUFFER PTNeighborOffsets
#include "Rtxdi/PT/Reservoir.hlsli"
typedef Payload RAB_RayPayload;
float RAB_RayPayloadGetCommittedHitT(RAB_RayPayload p){return p.t;}
RAB_Surface RAB_EmptySurface(){return (RAB_Surface)0;}
bool RAB_IsSurfaceValid(RAB_Surface s){return s.normalValid.w!=0;}
float3 RAB_GetSurfaceWorldPos(RAB_Surface s){return s.positionDepth.xyz;}
float3 RAB_GetSurfaceNormal(RAB_Surface s){return s.normalValid.xyz;}
float3 RAB_GetSurfaceViewDir(RAB_Surface s){return s.viewObject.xyz;}
float RAB_GetSurfaceRoughness(RAB_Surface s){return 1;}
float RAB_GetSurfaceLinearDepth(RAB_Surface s){return s.positionDepth.w;}
void RAB_SetSurfaceWorldPos(inout RAB_Surface s,float3 p){s.positionDepth.xyz=p;}
void RAB_SetSurfaceNormal(inout RAB_Surface s,float3 n){s.normalValid.xyz=n;}
float4 RAB_GetMaterial(RAB_Surface s){return s.albedoMaterial;}
bool RAB_AreMaterialsSimilar(float4 a,float4 b){return a.w==b.w&&all(abs(a.xyz-b.xyz)<.08);}
int2 RAB_ClampSamplePositionIntoView(int2 p,bool previous){return clamp(p,0,int2(Dimensions.xy)-1);}
uint ptSurfaceAddress(uint2 p,bool previous){return ((Dimensions.z&1)^(previous?1:0))*Dimensions.x*Dimensions.y+p.y*Dimensions.x+p.x;}
RAB_Surface RAB_GetGBufferSurface(int2 p,bool previous){
    if(any(p<0)||any(p>=int2(Dimensions.xy))||(previous&&!PTControls.y))return RAB_EmptySurface();
    return PTSurfaces[ptSurfaceAddress(p,previous)];
}
RAB_Surface ptSurface(Hit h,float3 n,float3 view,float3 albedo){
    RAB_Surface s;
    s.positionDepth=float4(h.p,max(.05,dot(h.p-CameraPosition.xyz,CameraForward.xyz)));
    s.normalValid=float4(n,1);s.albedoMaterial=float4(albedo,float(h.material));
    s.viewObject=float4(view,float(h.object));return s;
}
float RAB_SurfaceEvaluateBrdfPdf(RAB_Surface s,float3 d){return max(0,dot(s.normalValid.xyz,d))/PI;}
float3 ptTransmittance(RAB_Surface s,float3 p){
    return exp(-extinctionRgb(ambientMedium(s.positionDepth.xyz+s.normalValid.xyz*EPS*2))*length(p-s.positionDepth.xyz));
}
float3 RAB_GetReflectedBsdfRadianceForSurface(float3 p,float3 light,RAB_Surface s){
    float3 delta=p-s.positionDepth.xyz;float r2=dot(delta,delta);
    return r2>1e-10?light*s.albedoMaterial.xyz*RAB_SurfaceEvaluateBrdfPdf(s,delta*rsqrt(r2))*ptTransmittance(s,p):0;
}
float3 RAB_GetPTSampleTargetPdfForSurface(float3 p,float3 light,RAB_Surface s){return RAB_GetReflectedBsdfRadianceForSurface(p,light,s);}
bool RAB_GetConservativeVisibility(RAB_Surface s,float3 p){
    float3 delta=p-s.positionDepth.xyz;float distance=length(delta);
    return distance>EPS*4&&!occluded(s.positionDepth.xyz+s.normalValid.xyz*EPS*2,delta/distance,255,7,distance-EPS*4);
}
uint RAB_GetDuplicationMapCount(int2 p){return 0;} // Not enabled; bounded age + uniform decorrelation below.
struct RAB_PathTracerUserData { uint invocation; };
void RAB_PathTracerUserDataSetPathType(inout RAB_PathTracerUserData u,uint16_t t){u.invocation=t;}
void RAB_ReconnectionDenoiserCallback(RTXDI_PTReservoir r,RAB_Surface s,inout RAB_PathTracerUserData u){}
void RAB_LastBounceDenoiserCallback(float3 p,RAB_Surface s,inout RAB_PathTracerUserData u){}
// This bridge integrates all lab lights through the replayable, compensated NEE
// sampler, not RTXDI's optional separately resampled light-ID endpoint API.
// These type adapters satisfy that SDK API; no generated reservoir encodes NEE IDs.
typedef uint RAB_LightInfo;
struct RAB_LightSample { float4 position,radiance; };
RAB_LightSample RAB_EmptyLightSample(){return (RAB_LightSample)0;}
RAB_LightInfo RAB_LoadLightInfo(uint id,bool previous){return id;}
RAB_LightSample RAB_SamplePolymorphicLight(RAB_LightInfo l,RAB_Surface s,float2 uv){return RAB_EmptyLightSample();}
int RAB_TranslateLightIndex(uint id,bool previous){return -1;}
float3 RAB_LightSamplePosition(RAB_LightSample l){return l.position.xyz;}
float3 RAB_LightSampleRadiance(RAB_LightSample l){return l.radiance.xyz;}
float RAB_LightSampleSolidAnglePdf(RAB_LightSample l){return 1;}
bool RAB_GetConservativeVisibility(RAB_Surface s,RAB_LightSample l){return false;}
float RAB_GetMISWeightForNEE(uint id,RAB_LightSample l,float3 d,float p,float q){return 0;}
#define RAB_DISTANT_LIGHT_DISTANCE 100.0
#include "Rtxdi/PT/PathTracerContext.hlsli"

template<typename Context>
void RAB_PathTrace(inout RTXDI_PathTracerContext<Context> ctx,
                  inout RTXDI_PathTracerRandomContext rng,inout RAB_PathTracerUserData user){
    for(;ctx.GetBounceDepth()<=ctx.GetMaxPathBounce();ctx.IncreaseBounceDepth()){
        ctx.BeginPathState();RAB_Surface previous=ctx.GetIntersectionSurface();
        float u=RTXDI_GetNextRandom(rng.replayRandomSamplerState),v=RTXDI_GetNextRandom(rng.replayRandomSamplerState);
        float3 x,y;basis(previous.normalValid.xyz,x,y);
        float3 d=x*(sqrt(u)*cos(2*PI*v))+y*(sqrt(u)*sin(2*PI*v))+previous.normalValid.xyz*sqrt(1-u);
        RayDesc ray;ray.Origin=previous.positionDepth.xyz+previous.normalValid.xyz*EPS*2;
        ray.Direction=d;ray.TMin=EPS;ray.TMax=100;
        Payload p=trace(ray.Origin,d,255,ctx.GetBounceDepth());
        RTXDI_BrdfRaySample sample=(RTXDI_BrdfRaySample)0;
        sample.properties=RTXDI_DefaultBrdfRaySampleProperties();
        sample.outDirection=d;sample.outPdf=RAB_SurfaceEvaluateBrdfPdf(previous,d);
        // The SDK caches suffix throughput through the continuation BRDF term.
        // Fold segment transmittance into that term so both replay and cached
        // suffixes retain Beer attenuation (MultiplyPathThroughput alone does not).
        sample.brdfTimesNoL=previous.albedoMaterial.xyz*sample.outPdf*
            exp(-extinctionRgb(ambientMedium(ray.Origin))*p.t);
        ctx.SetBrdfRaySample(sample);
        if(!ctx.ValidContinuationRayBrdfOverPdf())break;
        ctx.MultiplyPathThroughput(ctx.GetContinuationRayBrdfOverPdf());
        ctx.SetContinuationRay(ray);
        if(!ctx.AnalyzePathReconnectibilityBeforeTrace())break;
        ctx.SetTraceResult(p);
        if(p.object==0xffffffff){
            ctx.RecordPathRadianceMiss(rng.initialRandomSamplerState);
            ctx.RecordEnvironmentMapLightSample(sky(d),previous,rng.initialRandomSamplerState);break;
        }
        Hit h=surface(p,ray.Origin,d);
        if(glass(h))break; // Photon ownership: never continue D -> S transport.
        float3 n=dot(h.n,d)<0?h.n:-h.n;
        ctx.RecordPathIntersection(ptSurface(h,n,-d,baseColor(h)));
        if(ctx.IsPathTerminated())break;
        // Advance even when replay skips intermediate lighting evaluations.
        uint seed=asuint(RTXDI_GetNextRandom(rng.replayRandomSamplerState));
        if(ctx.ShouldSampleEmissiveSurfaces()){
            // Keep the stochastic NEE oracle in replay space, separate from RIS
            // selection. A replayed path must reproduce the same light samples.
            float3 light=endpoint(h,n,seed);
            if(Play.x&&(h.object==3||h.object==4))light-=emission(h); // Already sampled by NEE.
            ctx.RecordEmissiveLightSample(max(0,light),previous,rng.initialRandomSamplerState);
        }
    }
}
#include "Rtxdi/PT/InitialSampling.hlsli"
#include "Rtxdi/PT/TemporalResampling.hlsli"
#include "Rtxdi/PT/SpatialResampling.hlsli"

RTXDI_ReservoirBufferParameters ptBuffers(){
    RTXDI_ReservoirBufferParameters b=(RTXDI_ReservoirBufferParameters)0;
    b.reservoirBlockRowPitch=((Dimensions.x+15)/16)*256;
    b.reservoirArrayPitch=b.reservoirBlockRowPitch*((Dimensions.y+15)/16);return b;
}
RTXDI_PTBufferIndices ptIndices(){
    RTXDI_PTBufferIndices b=(RTXDI_PTBufferIndices)0;
    b.initialPathTracerOutputBufferIndex=0;b.initialPathTracerPreservedBufferIndex=0;
    b.temporalResamplingInputBufferIndex=2+((Dimensions.z&1)^1);b.temporalResamplingOutputBufferIndex=1;
    b.spatialResamplingInputBufferIndex=1;b.spatialResamplingOutputBufferIndex=2+(Dimensions.z&1);
    b.finalShadingInputBufferIndex=b.spatialResamplingOutputBufferIndex;return b;
}
RTXDI_RuntimeParameters ptRuntime(){
    RTXDI_RuntimeParameters r=(RTXDI_RuntimeParameters)0;r.neighborOffsetMask=255;r.frameIndex=Dimensions.z;return r;
}
RTXDI_PTReconnectionParameters ptReconnection(){
    RTXDI_PTReconnectionParameters r=(RTXDI_PTReconnectionParameters)0;
    r.reconnectionMode=RTXDI_RESTIRPT_RECONNECTION_MODE_FOOTPRINT;
    r.minConnectionFootprint=.025;r.minConnectionFootprintSigma=.1;
    r.minPdfRoughness=.5;r.minPdfRoughnessSigma=.1;return r;
}
RTXDI_PTHybridShiftPerFrameParameters ptHybrid(){
    RTXDI_PTHybridShiftPerFrameParameters h=(RTXDI_PTHybridShiftPerFrameParameters)0;
    h.maxBounceDepth=3;h.maxRcVertexLength=4;return h;
}
void ptInitialize(uint2 pixel){
    PTSurfaces[ptSurfaceAddress(pixel,false)]=RAB_EmptySurface();
    RTXDI_StorePTReservoir(RTXDI_EmptyPTReservoir(),ptBuffers(),pixel,0);
}
void ptInitial(uint2 pixel,Hit h,float3 n,float3 view,float3 albedo){
    RAB_Surface s=ptSurface(h,n,view,albedo);PTSurfaces[ptSurfaceAddress(pixel,false)]=s;
    RTXDI_PTInitialSamplingParameters params=(RTXDI_PTInitialSamplingParameters)0;
    params.numInitialSamples=opticalSamples(pixel);params.maxBounceDepth=3;params.maxRcVertexLength=4;
    if(OpticalControls.x&1)OpticalInitialPaths+=params.numInitialSamples;
    RTXDI_PTInitialSamplingRuntimeParameters rt=(RTXDI_PTInitialSamplingRuntimeParameters)0;
    rt.cameraPos=CameraPosition.xyz;rt.prevCameraPos=rt.prevPrevCameraPos=PTPreviousCamera.xyz;
    RAB_PathTracerUserData user=(RAB_PathTracerUserData)0;
    RTXDI_PathTracerRandomContext rng=RTXDI_InitializePathTracerRandomContext(pixel,Dimensions.z,0x134a,0x79c2);
    RTXDI_PTReservoir r=GenerateInitialSamples(params,rt,ptReconnection(),rng,s,user);
    RTXDI_StorePTReservoir(r,ptBuffers(),pixel,0);
}
[shader("raygeneration")]
void PTTemporalRaygen(){
    uint2 pixel=DispatchRaysIndex().xy;RTXDI_PTReservoir r=RTXDI_LoadPTReservoir(ptBuffers(),pixel,0);
    if(PTControls.y){
        RTXDI_PTTemporalResamplingParameters p=(RTXDI_PTTemporalResamplingParameters)0;
        p.depthThreshold=.02;p.normalThreshold=.95;p.maxHistoryLength=PTControls.z;p.maxReservoirAge=4;
        p.enableAgeBasedRejection=1;p.enableVisibilityBeforeCombine=1;p.uniformRandomNumber=hash(Dimensions.z);
        RTXDI_PTTemporalResamplingRuntimeParameters rt=(RTXDI_PTTemporalResamplingRuntimeParameters)0;
        rt.pixelPosition=rt.reservoirPosition=pixel;
        RAB_Surface s=RAB_GetGBufferSurface(pixel,false);
        float4 previous=mul(float4(s.positionDepth.xyz,1),PreviousViewProjection);
        rt.motionVector=float3(Motion[pixel],previous.w-s.positionDepth.w);
        rt.cameraPos=CameraPosition.xyz;rt.prevCameraPos=rt.prevPrevCameraPos=PTPreviousCamera.xyz;
        RAB_PathTracerUserData user=(RAB_PathTracerUserData)0;bool selected=false;
        r=RTXDI_PTTemporalResampling(p,rt,ptHybrid(),ptReconnection(),ptRuntime(),ptBuffers(),
            RTXDI_InitRandomSampler(pixel,Dimensions.z,0x334),ptIndices(),selected,user);
    }
    RTXDI_StorePTReservoir(r,ptBuffers(),pixel,1);
    opticalEndReuse(4);
}
[shader("raygeneration")]
void PTSpatialRaygen(){
    uint2 pixel=DispatchRaysIndex().xy;RTXDI_PTReservoir r=RTXDI_LoadPTReservoir(ptBuffers(),pixel,1);
    RAB_Surface s=RAB_GetGBufferSurface(pixel,false);
    if(PTControls.w&&RAB_IsSurfaceValid(s)){
        RTXDI_PTSpatialResamplingParameters p=(RTXDI_PTSpatialResamplingParameters)0;
        p.numSpatialSamples=PTControls.w;p.numDisocclusionBoostSamples=PTControls.w;
        p.maxTemporalHistory=PTControls.z;p.samplingRadius=12;p.depthThreshold=.02;p.normalThreshold=.95;
        RTXDI_PTSpatialResamplingRuntimeParameters rt=(RTXDI_PTSpatialResamplingRuntimeParameters)0;
        rt.pixelPosition=rt.reservoirPosition=pixel;rt.viewportSize=Dimensions.xy;
        rt.cameraPos=CameraPosition.xyz;rt.prevCameraPos=rt.prevPrevCameraPos=PTPreviousCamera.xyz;
        RAB_PathTracerUserData user=(RAB_PathTracerUserData)0;bool selected=false;
        r=RTXDI_PTSpatialResampling(rt,p,ptHybrid(),ptReconnection(),ptBuffers(),ptIndices(),ptRuntime(),
            RTXDI_InitRandomSampler(pixel,Dimensions.z,0x965),selected,user);
    }
    RTXDI_StorePTReservoir(r,ptBuffers(),pixel,ptIndices().finalShadingInputBufferIndex);
    // RR consumes temporally decorrelated NOISY radiance: independently choose
    // a fresh estimator, never blend/pre-denoise the input or clamp its energy.
    RTXDI_RandomSamplerState rng=RTXDI_InitRandomSampler(pixel,Dimensions.z,0x874);
    if(RTXDI_GetNextRandom(rng)<.25)r=RTXDI_LoadPTReservoir(ptBuffers(),pixel,0);
    float3 light=r.targetFunction*r.weightSum;
    if(any(!isfinite(light))||any(light<0)) {Stats.InterlockedAdd(160,1);light=0;}
    if(RAB_IsSurfaceValid(s)){
        Stats.InterlockedAdd(164,1);
        if(r.M>1)Stats.InterlockedAdd(168,1);
        light*=exp(-extinctionRgb(ambientMedium(CameraPosition.xyz))*length(s.positionDepth.xyz-CameraPosition.xyz));
        Noisy[pixel]+=float4(light,0);
    }
    opticalEndReuse(8);
}
