#include "common.hlsli"
#include "work-shared.hlsli"
Texture2D<float> SceneDepth : register(t0);
struct Varying {float4 position:SV_Position;float2 uv:TEXCOORD0;float depth:TEXCOORD1;float3 color:TEXCOORD2;float4 clip:TEXCOORD3;};
Varying DebugVS(uint vertex:SV_VertexID,uint instance:SV_InstanceID) {
    static const float2 corners[6]={float2(-1,-1),float2(-1,1),float2(1,1),float2(-1,-1),float2(1,1),float2(1,-1)};
    FluidParticle p=(FluidParticle)0;float3 color=0;
    if(Display.x==0||Display.x==5){p=Particles[instance];
        float value=Display.x==5?log2(max(1,particleWeight(p)))/3:saturate(length(p.velocityFlags.xyz)/6);
        color=lerp(float3(.08,.65,1),float3(1,.24,.035),value);}
    else {
        uint3 c=cellFromIndex(instance);float4 field=Cells[instance];
        p.positionRadius=float4(DomainMinCell.xyz+(float3(c)+.5)*DomainMinCell.w,DomainMinCell.w*.5);
        p.velocityFlags.w=field.z;
        if(Display.x==7){
            uint faceTile=workIndex(c/workShape(),workFaceGrid()),stride=workProduct(workFaceGrid());
            bool gathered=SimulationWork[workFaceFlags()+faceTile]||SimulationWork[workFaceFlags()+stride+faceTile]||SimulationWork[workFaceFlags()+2*stride+faceTile];
            bool density=SimulationWork[workDensityFlags()+workIndex(c/workShape(),workDensityGrid())]!=0;
            p.velocityFlags.w=gathered||density;color=gathered?(density?float3(1,1,1):float3(0,.85,1)):float3(1,.4,.03);
        }
        else if(Display.x==6){p.velocityFlags.w=interiorCellQuanta(c,Grid.xyz,DomainMinCell)>0;color=float3(.08,.95,.9);}
        else if(Display.x==1) {
            float3 v;
            [unroll]for(uint a=0;a<3;++a){uint3 next=c;next[a]++;v[a]=.5*(Faces[faceIndex(c,a)].x+Faces[faceIndex(next,a)].x);}
            color=.5+.5*v/max(length(v),.001);
        }else if(Display.x==4)color=float3(.1,1,.4);
        else {float value=Display.x==2?field.w*.001:field.y*.5;color=lerp(float3(.12,.14,.18),value<0?float3(.1,.3,1):float3(1,.12,.02),saturate(abs(value)));}
    }
    float2 uv=corners[vertex];
    float3 world=p.positionRadius.xyz+(CameraRight.xyz*uv.x+CameraUp.xyz*uv.y)*p.positionRadius.w*.40;
    Varying o;o.position=mul(float4(world,1),ViewProjection);
    if(!p.velocityFlags.w)o.position=float4(0,0,-1,1);
    o.clip=o.position;o.uv=uv;o.depth=dot(world-CameraPosition.xyz,CameraForward.xyz);o.color=color;
    return o;
}
float4 DebugPS(Varying i):SV_Target {
    if(dot(i.uv,i.uv)>1)discard;
    uint w,h;SceneDepth.GetDimensions(w,h);
    float2 uv=i.clip.xy/i.clip.w*float2(.5,-.5)+.5;
    float depth=SceneDepth.Load(int3(clamp(int2(uv*float2(w,h)),0,int2(w-1,h-1)),0));
    if(Display.x!=6&&Display.x!=7&&i.depth>depth+.025)discard;
    return float4(i.color,1);
}
