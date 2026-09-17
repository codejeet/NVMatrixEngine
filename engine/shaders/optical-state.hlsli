#ifndef LAB_OPTICAL_STATE
#define LAB_OPTICAL_STATE
// Geometry/control are immutable after camera raygen. Finalization updates
// only moments/signals; neighboring threads never read those current writes.
struct OpticalPixel {
    float4 positionDepth,normalRoughness;
    uint4 control; // object, material, previous pixel or invalid, samples | ((fluid brick + 1) << 8)
    float4 moments; // compressed-luminance mean, second moment, observed sample, history length
    float4 signals; // importance, temporal change, receiver importance, confidence
};
RWStructuredBuffer<OpticalPixel> OpticalPixels : register(u16);
RWByteAddressBuffer OpticalCounters : register(u17);
RWStructuredBuffer<float4> OpticalReceivers : register(u18);
RWStructuredBuffer<uint2> OpticalFeedback : register(u19);
struct OpticalComplexityBrick {float4 importance,dynamics,velocity;uint4 state;};
RWStructuredBuffer<OpticalComplexityBrick> OpticalWorld : register(u20);
static uint OpticalRayCount=0;
static uint OpticalInitialPaths=0;
static bool OpticalPhysicsPrior=false;
static float OpticalReceiverSignal=0;
uint opticalAddress(uint2 pixel,bool previous){
    return ((Dimensions.z&1)^(previous?1:0))*Dimensions.x*Dimensions.y+pixel.y*Dimensions.x+pixel.x;
}
float opticalCompress(float x){return max(x,0)/(1+max(x,0));}
uint opticalChooseSamples(float importance,uint cap){
    return min(cap,importance<.25?1u:(importance<.6?2u:(importance<.85?4u:8u)));
}
float opticalReceiver(float2 uv,uint chart){
    if(!(OpticalControls.x&1)||chart>=5)return 0;
    float2 p=atlasPixel(uv,chart);int2 lo=int2(chart*uint(Atlas.z),0),hi=lo+int2(Atlas.z-1,Atlas.y-1);
    int2 q=int2(floor(p));float2 f=frac(p);float values[4];
    [unroll]for(uint k=0;k<4;++k){int2 t=clamp(q+int2(k&1,k>>1),lo,hi);
        values[k]=OpticalReceivers[t.y*uint(Atlas.x)+t.x].x;}
    return lerp(lerp(values[0],values[1],f.x),lerp(values[2],values[3],f.x),f.y);
}
void opticalRecordReceiver(float2 uv,uint chart){
    if(OpticalControls.x&1)OpticalReceiverSignal=max(OpticalReceiverSignal,opticalReceiver(uv,chart));
}
uint opticalBegin(uint2 pixel,float3 position,float3 oldPosition,float3 normal,float roughness,uint object,uint material,uint fluidBrick=0xffffffff){
    if(!(OpticalControls.x&1))return 1;
    OpticalPixel s=(OpticalPixel)0;
    s.positionDepth=float4(position,max(.05,dot(position-CameraPosition.xyz,CameraForward.xyz)));
    s.normalRoughness=float4(normal,roughness);s.control=uint4(object,material,0xffffffff,1);
    float4 clip=mul(float4(oldPosition,1),PreviousViewProjection);
    // Project directly to the previous JITTERED pixel lattice. Motion guides
    // stay unjittered for RR; their fp16 packing is not used for this test.
    float2 projected=(clip.xy/max(clip.w,1e-6)*float2(.5,-.5)+.5)*Dimensions.xy-.5-Jitter.zw;
    projected=clamp(projected,-float2(Dimensions.xy),2*float2(Dimensions.xy));
    int2 previous=int2(floor(projected+.5));float importance=1,confidence=0;
    if(OpticalControls.y&&object!=0xffffffff&&clip.w>0&&all(previous>=0)&&all(previous<int2(Dimensions.xy))){
        OpticalPixel h=OpticalPixels[opticalAddress(previous,true)];
        float footprint=2*CameraUp.w/Dimensions.y*length(position-CameraPosition.xyz);
        float tolerance=max(OpticalParameters.y,footprint*2.5);
        float error=length(h.positionDepth.xyz-oldPosition),cosine=dot(h.normalRoughness.xyz,normal);
        if(h.control.x==object&&h.control.y==material&&h.moments.w>0&&error<tolerance&&cosine>.94){
            s.control.z=previous.y*Dimensions.x+previous.x;
            confidence=saturate(h.moments.w/8)*saturate(1-error/tolerance)*saturate((cosine-.94)/.06);
            importance=max(h.signals.x,1-saturate(h.moments.w/8));
        }
    }
    // Current geometry can promote a region before radiance variance catches up.
    float materialImportance=roughness<.1?.55:0;
    importance=max(importance,materialImportance);
    uint worldAddress=0;
    if((OpticalControls.x&8)&&fluidBrick<FluidBricks.w){
            // The actual DXR primitive owns the interface, including exact brick
            // borders. Do not independently re-quantize the float hit position.
            worldAddress=fluidBrick+1;
            OpticalComplexityBrick brick=OpticalWorld[fluidBrick];
            float physics=saturate(max(brick.dynamics.x,max(brick.dynamics.y,brick.dynamics.z)/8));
            importance=max(importance,physics);
            OpticalPhysicsPrior=physics>.1;
    }
    s.signals=float4(importance,0,0,confidence);
    uint samples=1;
    if(OpticalControls.x&2)samples=opticalChooseSamples(importance,OpticalControls.z);
    if(OpticalControls.w&256)samples=OpticalControls.z;
    if((OpticalControls.x&4)&&OpticalControls.y){
        OpticalPixel frozen=OpticalPixels[opticalAddress(pixel,true)];
        samples=max(1u,frozen.control.w&255u);s.signals.x=frozen.signals.x;
    }
    if(object==0xffffffff)samples=0;
    s.control.w=samples|(worldAddress<<8);OpticalPixels[opticalAddress(pixel,false)]=s;
    return samples;
}
void opticalEndCamera(uint2 pixel){
    if(!(OpticalControls.x&1))return;
    OpticalPixels[opticalAddress(pixel,false)].signals.z=OpticalReceiverSignal;
    uint rays=WaveActiveSum(OpticalRayCount);
    uint paths=WaveActiveSum(OpticalInitialPaths),physics=WaveActiveCountBits(OpticalPhysicsPrior);
    if(WaveIsFirstLane()){
        OpticalCounters.InterlockedAdd(0,rays);
        if(paths)OpticalCounters.InterlockedAdd(16,paths);
        if(physics)OpticalCounters.InterlockedAdd(60,physics);
    }
}
void opticalEndReuse(uint counter){
    if(!(OpticalControls.x&1))return;
    uint rays=WaveActiveSum(OpticalRayCount);
    if(WaveIsFirstLane())OpticalCounters.InterlockedAdd(counter,rays);
}
uint opticalSamples(uint2 pixel){return (OpticalControls.x&1)?OpticalPixels[opticalAddress(pixel,false)].control.w&255u:1;}
#endif
