#include "../common.hlsli"
struct Payload {float t;uint primitive,object,bary;};
#include "intersection.hlsli"
struct Hit {float3 p,n;uint material;};
#include "../interface-media.hlsli"
struct TestRay {float4 originMin,directionMax;uint4 control;};
struct TestResult {float4 hit,media,optics,attenuation,continuation,receiver;};
StructuredBuffer<TestRay> TestRays : register(t9);
RWStructuredBuffer<TestResult> TestResults : register(u24);
[shader("miss")]
void ProbeMiss(inout Payload p){p.t=RayTCurrent();p.object=0xffffffff;p.primitive=0;p.bary=0;}
Payload probeTrace(RayDesc ray,uint mask,bool visibility) {
    Payload p;p.t=ray.TMax;p.object=visibility?0:0xffffffff;p.primitive=0;p.bary=0;
    uint flags=RAY_FLAG_FORCE_OPAQUE|(visibility?(RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH|RAY_FLAG_SKIP_CLOSEST_HIT_SHADER):0);
    TraceRay(Scene,flags,mask,0,0,0,ray,p);
    return p;
}
[shader("raygeneration")]
void SurfaceRaygen() {
    uint id=DispatchRaysIndex().x;
    TestRay input=TestRays[id];RayDesc ray;
    ray.Origin=input.originMin.xyz;ray.TMin=input.originMin.w;
    ray.Direction=input.directionMax.xyz;ray.TMax=input.directionMax.w;
    TestResult r=(TestResult)0;r.hit.x=-1;r.continuation.x=-1;r.receiver.w=-1;
    uint originMedium=ambientMedium(ray.Origin);
    r.media.x=float(originMedium);
    Payload p=probeTrace(ray,input.control.x,false),shadow=probeTrace(ray,input.control.x,true);
    r.media.w=shadow.object!=0xffffffff?1:0;
    if(p.object!=0xffffffff) {
        Hit h;h.p=ray.Origin+ray.Direction*p.t;h.n=liquidNormal(h.p);h.material=8;
        r.hit=float4(p.t,h.n);
        r.media.y=float(ambientMedium(h.p+h.n*EPS*4));
        r.media.z=float(ambientMedium(h.p-h.n*EPS*4));
        uint next;float ni,nt;interfaceMedia(h,ray.Direction,550,next,ni,nt);
        float3 normal=dot(h.n,ray.Direction)<0?h.n:-h.n;
        float F=fresnel(saturate(-dot(normal,ray.Direction)),ni,nt);
        float3 transmitted=refract(ray.Direction,normal,ni/nt);
        r.optics=float4(F,transmitted);
        r.attenuation=float4(exp(-extinctionRgb(originMedium)*p.t),float(next));
        if(next==8&&dot(transmitted,transmitted)>0) {
            RayDesc through;through.Origin=h.p;through.TMin=EPS;
            through.Direction=transmitted;through.TMax=100;
            Payload exit=probeTrace(through,8,false);
            if(exit.object!=0xffffffff) {
                Hit e;e.p=through.Origin+through.Direction*exit.t;e.n=liquidNormal(e.p);e.material=8;
                uint after;float ei,et;interfaceMedia(e,through.Direction,550,after,ei,et);
                r.continuation=float4(exit.t,exp(-extinction(8,550)*exit.t),float(after),1);
                float3 en=dot(e.n,through.Direction)<0?e.n:-e.n;
                float3 air=refract(through.Direction,en,ei/et);
                if(after==0&&air.y<0&&e.p.y>-.5){
                    float t=(-.5-e.p.y)/air.y;
                    float exitF=fresnel(saturate(-dot(en,through.Direction)),ei,et);
                    RayDesc receiverRay;receiverRay.Origin=e.p;receiverRay.Direction=air;
                    receiverRay.TMin=EPS;receiverRay.TMax=t-EPS;
                    Payload blocked=probeTrace(receiverRay,8,true);
                    // Unit incident photon flux on an infinite diffuse receiver.
                    // No radiance eta-squared factor belongs in this flux test.
                    if(blocked.object==0xffffffff)r.receiver=float4(e.p+air*t,(1-F)*(1-exitF)*r.continuation.y);
                }
            }
        }
    }
    TestResults[id]=r;
}
