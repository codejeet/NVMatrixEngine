#include "common.hlsli"
float opticalGeometryEdge(uint2 pixel,OpticalPixel s){
    float edge=0;const int2 offsets[4]={int2(-1,0),int2(1,0),int2(0,-1),int2(0,1)};
    [unroll]for(uint k=0;k<4;++k){int2 p=int2(pixel)+offsets[k];
        if(any(p<0)||any(p>=int2(Dimensions.xy)))continue;
        uint address=opticalAddress(p,false);
        uint2 identity=OpticalPixels[address].control.xy;
        float4 position=OpticalPixels[address].positionDepth,normal=OpticalPixels[address].normalRoughness;
        if(any(identity!=s.control.xy)){edge=1;continue;}
        edge=max(edge,saturate((1-dot(normal.xyz,s.normalRoughness.xyz))/.06));
        edge=max(edge,saturate(abs(position.w-s.positionDepth.w)/max(.02*s.positionDepth.w,.02)));
    }
    return edge;
}
[numthreads(8,8,1)]
void OpticalFinalize(uint3 id:SV_DispatchThreadID){
    if(any(id.xy>=Dimensions.xy))return;
    uint index=opticalAddress(id.xy,false);OpticalPixel s=OpticalPixels[index],old=(OpticalPixel)0;
    bool valid=s.control.z!=0xffffffff;
    if(valid)old=OpticalPixels[((Dimensions.z&1)^1)*Dimensions.x*Dimensions.y+s.control.z];
    float luma=dot(Noisy[id.xy].xyz,float3(.2126,.7152,.0722));
    float value=opticalCompress(luma),age=valid?min(old.moments.w+1,32):1,alpha=1/age;
    float mean=lerp(old.moments.x,value,alpha),square=lerp(old.moments.y,value*value,alpha);
    float variance=max(0,square-mean*mean);
    float temporal=valid?saturate(abs(value-old.moments.x)/(.05+value+old.moments.x)):1;
    float score=max(variance/(variance+OpticalParameters.z),max(temporal*2,opticalGeometryEdge(id.xy,s)));
    score=max(score,max(s.normalRoughness.w<.1?.55:0,max(s.signals.z*.8,1-s.signals.w)));
    score=saturate(max(score,old.signals.x*exp(-max(OpticalParameters.x,0)/.25)));
    if((OpticalControls.x&4)&&OpticalControls.y)score=OpticalPixels[opticalAddress(id.xy,true)].signals.x;
    float4 moments=float4(mean,square,value,age),signals=float4(score,temporal,s.signals.z,s.signals.w);
    if(any(!isfinite(moments))||any(!isfinite(signals))){OpticalCounters.InterlockedAdd(48,1);moments=float4(0,0,0,1);signals=1;}
    OpticalPixels[index].moments=moments;OpticalPixels[index].signals=signals;
    uint worldAddress=s.control.w>>8;
    bool sameBrick=WaveActiveAllEqual(worldAddress);
    float opticalError=signals.x,change=max(signals.y,signals.z);
    float maxError=WaveActiveMax(opticalError),maxChange=WaveActiveMax(change);
    // Most water tiles hit one brick. Reduce identical-key atomic contention
    // without a variable-length subgroup loop at splashes/brick silhouettes.
    if((OpticalControls.x&8)&&worldAddress&&(!sameBrick||WaveIsFirstLane())){
        uint brick=worldAddress-1;
        InterlockedMax(OpticalFeedback[brick].x,asuint(sameBrick?maxError:opticalError));
        InterlockedMax(OpticalFeedback[brick].y,asuint(sameBrick?maxChange:change));
    }
    bool hit=s.control.x!=0xffffffff;uint samples=s.control.w&255u;
    uint hitCount=WaveActiveCountBits(hit),sampleCount=WaveActiveSum(hit?samples:0);
    // HLSL 2021 short-circuits &&: form lane predicates without divergent
    // evaluation so every vote executes with the same active wave membership.
    uint hitBit=uint(hit);
    uint rejected=WaveActiveCountBits((hitBit&uint(!valid))!=0);
    uint causticCount=WaveActiveCountBits((hitBit&uint(signals.z>.05))!=0);
    uint feedbackCount=WaveActiveCountBits(worldAddress!=0),maxAge=WaveActiveMax(uint(age));
    uint4 buckets=uint4(WaveActiveCountBits((hitBit&uint(samples<=1))!=0),WaveActiveCountBits((hitBit&uint(samples==2))!=0),
        WaveActiveCountBits((hitBit&uint(samples>2)&uint(samples<=4))!=0),WaveActiveCountBits((hitBit&uint(samples>4))!=0));
    if(WaveIsFirstLane()){
        OpticalCounters.InterlockedAdd(12,hitCount);OpticalCounters.InterlockedAdd(20,sampleCount);
        OpticalCounters.InterlockedAdd(24,rejected);OpticalCounters.InterlockedAdd(44,causticCount);
        OpticalCounters.InterlockedAdd(56,feedbackCount);OpticalCounters.InterlockedMax(52,maxAge);
        [unroll]for(uint k=0;k<4;++k)if(buckets[k])OpticalCounters.InterlockedAdd(28+4*k,buckets[k]);
    }
    uint mode=OpticalControls.w&255u;
    if(mode){
        float v=mode==1?score:(mode==2?variance/(variance+OpticalParameters.z):(mode==3?temporal:
            (mode==4?signals.z:(mode==5?float(samples)/OpticalControls.z:signals.w))));
        float3 color=lerp(float3(.035,.15,.8),float3(1,.12,.025),saturate(v));
        if(!hit)color=.025;
        Noisy[id.xy]=float4(color,1);Albedo[id.xy]=1;SpecularAlbedo[id.xy]=0;NormalRoughness[id.xy].w=1;
    }
}
