// One-way diffuse phase inspired by Ihmsen et al. (2012). The APIC carrier is
// unchanged. Bounded birth/death, MAC advection and buoyancy, no secondary pairs.
cbuffer WhitewaterFrame:register(b0) {
    float4 FluidMinimumSpacing,DomainMinimum,DomainMaximum;
    uint4 FluidBricks,Grid,Control; // live carrier count, solver step, reset, aerated inlet
    float4 TimeGravity,Emitter;
};
#include "field.hlsli"
#define FLUID_COLLIDER_REGISTER t6
#define FLUID_MESH_REGISTER t7
#include "solid.hlsli"
struct Secondary {float4 positionRadius,velocityLife,previousType,surfaceAge;};
struct Carrier {float4 positionRadius,velocityFlags,c0,c1,c2;};
struct Aabb {float3 minimum,maximum;};
RWStructuredBuffer<Secondary> SecondaryParticles:register(u0);
RWStructuredBuffer<Aabb> Bounds:register(u1);
RWByteAddressBuffer Counters:register(u2);
RWStructuredBuffer<Carrier> Carriers:register(u3);
RWStructuredBuffer<float4> Faces:register(u4);
RWStructuredBuffer<float4> MaterialGrid:register(u5);
RWStructuredBuffer<uint> FoamDensity:register(u6);
// x: accumulated foam optical depth. yzw: density-weighted material displacement.
// Separate read/write history makes advection independent of dispatch order.
RWStructuredBuffer<float4> FoamPrevious:register(u7);
RWStructuredBuffer<float4> FoamNext:register(u8);
uint wwHash(uint s){s^=s>>16;s*=0x7feb352d;s^=s>>15;s*=0x846ca68b;return s^(s>>16);}
float wwRandom(inout uint s){s=wwHash(s);return (s>>8)*(1.0/16777216.0);}
float3 gridVelocity(float3 p) {
    float3 result=0;
    uint stride=(Grid.x+1)*(Grid.y+1)*(Grid.z+1);
    [unroll]for(uint axis=0;axis<3;++axis) {
        float3 offset=.5;offset[axis]=0;
        float3 g=(p-DomainMinimum.xyz)/DomainMinimum.w-offset;
        int3 base=int3(floor(g));float3 f=frac(g);float value=0,weight=0;
        [unroll]for(uint k=0;k<8;++k) {
            uint3 bit=uint3(k&1,(k>>1)&1,k>>2);
            int3 cell=base+int3(bit),extent=int3(Grid.xyz);extent[axis]++;
            if(any(cell<0)||any(cell>=extent))continue;
            float4 face=Faces[axis*stride+(cell.z*(Grid.y+1)+cell.y)*(Grid.x+1)+cell.x];
            float3 w=lerp(1-f,f,float3(bit));float mass=w.x*w.y*w.z*face.w;
            value+=mass*face.x;weight+=mass;
        }
        result[axis]=weight>1e-5?value/weight:0;
    }
    return result;
}
float3 surfaceGradient(float3 p) {
    float e=FluidMinimumSpacing.w*.5;float3 n;
    [unroll]for(uint a=0;a<3;++a){float3 v=0;v[a]=e;n[a]=(liquidSample(p+v).x-liquidSample(p-v).x)/(2*e);}
    return n;
}
float3 attachFoam(float3 p,float radius) {
    [unroll]for(uint iteration=0;iteration<2;++iteration) {
        float phi=liquidSample(p).x;float3 gradient=surfaceGradient(p);
        float m=length(gradient);if(m<.02)break;
        p-=gradient/m*clamp(phi/m,-FluidMinimumSpacing.w*.5,FluidMinimumSpacing.w*.5);
    }
    return p;
}
bool submerged(float3 p,float radius) {
    // The reconstruction is not a distance bound. Test the bubble's support,
    // not just its centre, before assigning the water -> air interface.
    if(liquidSample(p).x>=0)return false;
    [unroll]for(uint a=0;a<3;++a) {
        float3 v=0;v[a]=radius*1.5;
        if(liquidSample(p+v).x>=0||liquidSample(p-v).x>=0)return false;
    }
    return true;
}
[numthreads(8,1,1)]
void WhitewaterClear(uint id:SV_GroupIndex){Counters.Store(id*4,0);}
[numthreads(128,1,1)]
void WhitewaterUpdate(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x;if(id>=8192)return;
    Secondary p=(Secondary)0;if(!Control.z)p=SecondaryParticles[id];
    float dt=TimeGravity.x,h=FluidMinimumSpacing.w;
    uint rng=wwHash(id+Control.y*0x517cc1b7);
    p.previousType.xyz=p.positionRadius.xyz;
    bool alive=p.velocityLife.w>0,born=false;
    if(alive&&dt>0) {
        p.velocityLife.w-=dt;
        p.surfaceAge.w+=dt;
        float3 u=gridVelocity(p.positionRadius.xyz);
        uint kind=uint(p.previousType.w);
        if(kind==3)p.velocityLife.xyz+=TimeGravity.yzw*dt;
        else {
            // Quadratic-drag terminal rise, bounded to the observed small-bubble
            // regime. This is a subgrid approximation, not resolved two-phase CFD.
            float rise=kind==2?min(.3,sqrt(8*9.81*p.positionRadius.w/(3*.8))):0;
            p.velocityLife.xyz=lerp(p.velocityLife.xyz,u+float3(0,rise,0),1-exp(-dt*(kind==2?6:14)));
        }
        p.positionRadius.xyz+=p.velocityLife.xyz*dt;
        float phi=liquidSample(p.positionRadius.xyz).x;
        if(kind==2&&!submerged(p.positionRadius.xyz,p.positionRadius.w)){
            kind=1;p.surfaceAge.w=0;p.velocityLife.w=min(p.velocityLife.w,2.5);
        }
        if(kind==1) {
            if(phi>h*.8)kind=3;
            else p.positionRadius.xyz=attachFoam(p.positionRadius.xyz,p.positionRadius.w);
        }
        if(kind==3&&phi<0)p.velocityLife.w=0;
        p.previousType.w=kind;
        // Reuse actual scene SDFs. No bubbles/spray advect through rigid objects.
        for(uint i=0;i<uint(DomainMaximum.w);++i) {
            FluidCollider c=Colliders[i];float d=colliderPhi(c,p.positionRadius.xyz);
            if(d<p.positionRadius.w) {
                float e=DomainMinimum.w*.01;float3 n;
                [unroll]for(uint a=0;a<3;++a){float3 v=0;v[a]=e;n[a]=colliderPhi(c,p.positionRadius.xyz+v)-colliderPhi(c,p.positionRadius.xyz-v);}
                n=dot(n,n)>1e-14?normalize(n):float3(0,1,0);
                p.positionRadius.xyz+=n*(p.positionRadius.w-d);
                p.velocityLife.xyz-=n*min(0,dot(p.velocityLife.xyz,n));
            }
        }
        if(any(p.positionRadius.xyz<DomainMinimum.xyz)||any(p.positionRadius.xyz>DomainMaximum.xyz))p.velocityLife.w=0;
        if(p.velocityLife.w<=0){p=(Secondary)0;Counters.InterlockedAdd(16,1);alive=false;}
    }
    if(!alive&&dt>0&&Control.x) {
        Carrier c=Carriers[wwHash(rng)%Control.x];
        float speed=length(c.velocityFlags.xyz),phi=liquidSample(c.positionRadius.xyz).x;
        float3 u=gridVelocity(c.positionRadius.xyz);
        float deformation=DomainMinimum.w*(length(c.c0.xyz)+length(c.c1.xyz)+length(c.c2.xyz));
        uint3 cell=min(uint3(max(0,(c.positionRadius.xyz-DomainMinimum.xyz)/DomainMinimum.w)),Grid.xyz-1);
        float curvature=MaterialGrid[(cell.z*Grid.y+cell.y)*Grid.x+cell.x].y;
        float interfaceWeight=1-saturate(abs(phi)/(h*1.5));
        float agitation=saturate(length(c.velocityFlags.xyz-u)*.6+deformation*.15+max(curvature,0)*h*.25);
        // An aerated wall nozzle seeds entrained air only in its moving jet.
        if(Control.w&&distance(c.positionRadius.xyz,Emitter.xyz)<Emitter.w*6)agitation=max(agitation,.7);
        // Bounded secondary effect: weight the proposal by carrier rest mass,
        // not its sample multiplicity. Saturation still enforces this emitter's
        // one-secondary-per-slot budget; it is not a fluid mass transfer.
        float probability=saturate(smoothstep(.6,3,speed)*interfaceWeight*agitation*dt*12*c.c0.w);
        if(c.velocityFlags.w&&wwRandom(rng)<probability) {
            float3 jitter=float3(wwRandom(rng),wwRandom(rng),wwRandom(rng))-.5;
            p.positionRadius=float4(c.positionRadius.xyz+jitter*h*.6,lerp(.008,.021,wwRandom(rng)));
            phi=liquidSample(p.positionRadius.xyz).x;
            uint kind=submerged(p.positionRadius.xyz,p.positionRadius.w)?2:(phi<h*.4?1:3);
            p.velocityLife=float4(c.velocityFlags.xyz,kind==2?lerp(3,6,wwRandom(rng)):lerp(1.2,3,wwRandom(rng)));
            if(kind==1)p.positionRadius.xyz=attachFoam(p.positionRadius.xyz,p.positionRadius.w);
            p.previousType=float4(p.positionRadius.xyz,kind);
            p.surfaceAge=float4(0,1,0,0);
            alive=true;born=true;
            for(uint collider=0;collider<uint(DomainMaximum.w);++collider)
                if(colliderPhi(Colliders[collider],p.positionRadius.xyz)<p.positionRadius.w)alive=false;
            if(any(p.positionRadius.xyz<DomainMinimum.xyz)||any(p.positionRadius.xyz>DomainMaximum.xyz))alive=false;
            if(!alive)p=(Secondary)0;
        }
    }
    if(alive&&dt>0) {
        if(uint(p.previousType.w)==1) {
            // Collision projection can detach a marker; do not leave a foam
            // coating suspended in air or deep inside the liquid.
            float phi=liquidSample(p.positionRadius.xyz).x;
            if(phi>h*.5){p.previousType.w=3;p.surfaceAge.w=0;}
            else if(phi<-h*.5&&submerged(p.positionRadius.xyz,p.positionRadius.w)){
                p.previousType.w=2;p.surfaceAge.w=0;
            }else p.surfaceAge.xyz=liquidNormal(p.positionRadius.xyz);
        }
        bool blocked=false;
        for(uint collider=0;collider<uint(DomainMaximum.w)&&!blocked;++collider)
            blocked=colliderPhi(Colliders[collider],p.positionRadius.xyz)<p.positionRadius.w-1e-5;
        // Collision projection can move an existing bubble out of its medium.
        // Expire it rather than trace a water/air cavity in open air for a frame.
        if(uint(p.previousType.w)==2&&!submerged(p.positionRadius.xyz,p.positionRadius.w))blocked=true;
        if(blocked){if(!born)Counters.InterlockedAdd(16,1);p=(Secondary)0;alive=false;}
    }
    if(any(!isfinite(p.positionRadius))||any(!isfinite(p.velocityLife))||any(!isfinite(p.previousType))||any(!isfinite(p.surfaceAge))) {
        Counters.InterlockedAdd(20,1);p=(Secondary)0;alive=false;
    }
    Aabb box;box.minimum=float3(asfloat(0x7fc00000),0,0);box.maximum=0;
    if(alive) {
        if(born)Counters.InterlockedAdd(12,1);
        // Keep foam markers out of the procedural BLAS entirely. Only actual
        // air cavities and airborne droplets retain spherical boundaries.
        if(uint(p.previousType.w)!=1){
            box.minimum=p.positionRadius.xyz-p.positionRadius.w;box.maximum=p.positionRadius.xyz+p.positionRadius.w;
        }
        Counters.InterlockedAdd((uint(p.previousType.w)-1)*4,1);
    }
    SecondaryParticles[id]=p;Bounds[id]=box;
}
[numthreads(128,1,1)]
void FoamClear(uint3 tid:SV_DispatchThreadID) {
    uint3 size=FluidBricks.xyz*8+1;
    if(tid.x<size.x*size.y*size.z)FoamDensity[tid.x]=0;
}
[numthreads(128,1,1)]
void FoamSplat(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x;if(id>=8192)return;
    Secondary p=SecondaryParticles[id];
    if(p.velocityLife.w<=0||uint(p.previousType.w)!=1)return;
    float fade=smoothstep(0,.18,p.surfaceAge.w)*smoothstep(0,.65,p.velocityLife.w);
    if(fade<=0)return;
    float h=FluidMinimumSpacing.w;
    float3 n=p.surfaceAge.xyz,u=p.velocityLife.xyz;
    u-=n*dot(u,n);float speed=length(u);
    float3 tangent=speed>.03?u/speed:normalize(cross(n,abs(n.y)<.9?float3(0,1,0):float3(1,0,0)));
    float3 bitangent=cross(n,tangent);
    // Markers seed a shared entrainment field; they are NOT visible patches.
    // Buoyant surface rafts accumulate on upward interfaces, not the sides of
    // a falling jet. Underwater aeration keeps its separate bubble geometry.
    fade*=smoothstep(.1,.65,n.y);
    float radius=max(max(.065,p.positionRadius.w*4),h*1.1);
    float3 axes=float3(2.75*radius*(1+1.5*saturate(speed/2)),2.75*radius,1.75*h);
    float3 extent=sqrt(tangent*tangent*axes.x*axes.x+bitangent*bitangent*axes.y*axes.y+n*n*axes.z*axes.z);
    uint3 size=FluidBricks.xyz*8+1;
    int3 lo=max(0,int3(ceil((p.positionRadius.xyz-extent-FluidMinimumSpacing.xyz)/h)));
    int3 hi=min(int3(size)-1,int3(floor((p.positionRadius.xyz+extent-FluidMinimumSpacing.xyz)/h)));
    for(int z=lo.z;z<=hi.z;++z)for(int y=lo.y;y<=hi.y;++y)for(int x=lo.x;x<=hi.x;++x){
        float3 d=FluidMinimumSpacing.xyz+float3(x,y,z)*h-p.positionRadius.xyz;
        float3 q=float3(dot(d,tangent),dot(d,bitangent),dot(d,n))/axes;
        float w=saturate(1-dot(q,q));
        // Lighter surface coating keeps the aerated stream clear rather than
        // looking dyed. This does not change underwater bubbles or spray.
        // Per-marker optical depth <= .50, so even 8192 coincident markers cannot
        // overflow uint32. Integer accumulation is order-independent when paused.
        uint value=uint(round(65536*.50*fade*w*w*w));
        if(value)InterlockedAdd(FoamDensity[(z*size.y+y)*size.x+x],value);
    }
}
float4 foamHistorySample(float3 world) {
    float3 g=(world-FluidMinimumSpacing.xyz)/FluidMinimumSpacing.w;
    uint3 size=FluidBricks.xyz*8+1;
    if(any(g<0)||any(g>=float3(size-1)))return 0;
    uint3 p=uint3(floor(g));float3 f=frac(g);float4 value=0;
    [unroll]for(uint k=0;k<8;++k){uint3 b=uint3(k&1,(k>>1)&1,k>>2),q=p+b;
        float3 w=lerp(1-f,f,float3(b));value+=FoamPrevious[(q.z*size.y+q.y)*size.x+q.x]*w.x*w.y*w.z;}
    return value;
}
[numthreads(128,1,1)]
void FoamTransport(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x;uint3 size=FluidBricks.xyz*8+1;
    if(id>=size.x*size.y*size.z)return;
    uint3 q=uint3(id%size.x,(id/size.x)%size.y,id/(size.x*size.y));
    float h=FluidMinimumSpacing.w,dt=TimeGravity.x;float4 result=0;
    if(dt<=0&&!Control.z)result=FoamPrevious[id]; // exact pause, including material coordinates
    else {
        uint3 brick=min(q/8,FluidBricks.xyz-1);uint slot=LiquidBrickMap[liquidBrickIndex(brick)];
        float phi=slot==0xffffffff?h*3:liquidNode(slot,q-brick*8).x;
        if(abs(phi)<h*2.5) {
            float3 world=FluidMinimumSpacing.xyz+float3(q)*h;
            float3 back=world-gridVelocity(world)*dt;
            float4 old=Control.z?0:foamHistorySample(back);
            float source=float(FoamDensity[id])*(1.0/65536);
            // One marker has source <= .5 and can never create a fuzzy disc.
            // Overlapping entrainment grows a continuous, advected layer.
            float production=3.5*smoothstep(.5,1.8,source);
            float decay=exp(-dt/1.4);
            float density=min(4,old.x*decay+production*2*1.4*(1-decay));
            float3 displacement=old.x>1e-5?old.yzw/old.x+back-world:0;
            // Keep a finite, broad-band extension around the moving interface.
            // Shading still occurs ONLY on the canonical liquid zero crossing.
            if(density>1e-5)result=float4(density,displacement*density);
        }
    }
    if(any(!isfinite(result))){Counters.InterlockedAdd(20,1);result=0;}
    if(result.x>0){Counters.InterlockedAdd(24,1);Counters.InterlockedMax(28,uint(round(result.x*65536)));}
    FoamNext[id]=result;
}
