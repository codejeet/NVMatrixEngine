// Deterministic centre paths for visible laser single scattering. Surface
// illumination is still photon-owned: these cylinders never paint light spots.
// Narrow-beam envelope approximation; no diffraction, coherence or speckle.
[shader("raygeneration")]
void BeamRaygen() {
    uint laser=DispatchRaysIndex().x,base=laser*16;
    for(uint i=0;i<16;++i)Beams[base+i]=(Beam)0;
    Source s=source(laser+1);if(!Play.x||s.power<=0)return;
    float3 origins[8],directions[8];float powers[8],radii[8];uint media[8],depths[8];
    origins[0]=s.o;directions[0]=s.d;powers[0]=s.power;radii[0]=s.halfSize.x;
    media[0]=ambientMedium(s.o);depths[0]=0;uint pending=1;
    for(uint segment=0;segment<16&&pending;++segment) {
        uint k=--pending,medium=media[k],depth=depths[k];
        float3 o=origins[k],d=directions[k];float power=powers[k],radius=radii[k];
        Payload p=trace(o,d,255,laser,30);
        Beam b;b.originRadius=float4(o,radius);b.directionLength=float4(d,p.t);
        b.powerSigma=float4(spectralPower(s.nm,power),extinction(medium,s.nm));
        b.medium=float4(medium==8?Medium.y:(medium?0:Medium.x),medium==8?0:.25,medium,0);
        Beams[base+segment]=b;Stats.InterlockedAdd(68,1);
        // Compact masks retain the original segment/summation order. Camera
        // paths in glass have no scattering; most air/water paths need only a
        // few live cylinders, not 32 full Beam loads at every specular vertex.
        if(b.directionLength.w>0&&b.medium.x>0)Stats.InterlockedOr(medium==8?BEAM_WATER_MASK:BEAM_AIR_MASK,1u<<(base+segment));
        if(p.object==0xffffffff)continue;
        Hit h=surface(p,o,d);
        if(!glass(h)) {b.medium.w=float(h.object+1);Beams[base+segment]=b;Stats.InterlockedOr(BEAM_ENDPOINT_MASK,1u<<(base+segment));continue;}
        power*=exp(-extinction(medium,s.nm)*p.t);
        if(depth>=8){Stats.InterlockedAdd(72,1);continue;}
        uint next;float ni,nt;interfaceMedia(h,d,s.nm,next,ni,nt);
        float3 n=dot(h.n,d)<0?h.n:-h.n;float ci=saturate(-dot(n,d));
        float F=fresnel(ci,ni,nt),transmitted=power*(1-F)*(1-h.foam),reflected=power*F;
        if(h.foam>1e-5) {
            // The coating receives the removed beam fraction through its local
            // diffuse material; continuation carries only uncovered transmission.
            b.medium.w=float(h.object+1);Beams[base+segment]=b;
            Stats.InterlockedOr(BEAM_ENDPOINT_MASK,1u<<(base+segment));
        }
        if(transmitted>.0001&&pending<8) {
            float3 td=normalize(refract(d,n,ni/nt));
            origins[pending]=h.p+td*EPS*2;directions[pending]=td;powers[pending]=transmitted;
            radii[pending]=clamp(radius*sqrt(abs(dot(td,n))/max(ci,.01)),.002,.15);
            media[pending]=next;depths[pending++]=depth+1;
        }
        if(reflected>.0001&&pending<8) {
            float3 rd=reflect(d,n);origins[pending]=h.p+rd*EPS*2;directions[pending]=rd;
            powers[pending]=reflected;radii[pending]=radius;media[pending]=medium;depths[pending++]=depth+1;
        }
    }
    if(pending)Stats.InterlockedAdd(72,pending);
}
float3 laserIrradiance(Hit h,float3 n) {
    // Charted room/tank receivers are photon-owned. Uncharted props use the
    // narrow centre-beam envelope as a bounded spot approximation instead.
    if(!Play.x||!Water.z||h.chart<5)return 0;
    float3 result=0;
    for(uint candidates=Stats.Load(BEAM_ENDPOINT_MASK);candidates;candidates&=candidates-1) {
        uint i=firstbitlow(candidates);
        Beam b=Beams[i];if(b.medium.w!=float(h.object+1))continue;
        float3 v=h.p-b.originRadius.xyz;float t=dot(v,b.directionLength.xyz);
        float3 radial=v-t*b.directionLength.xyz;
        if(t<=0||abs(t-b.directionLength.w)>b.originRadius.w*16||dot(radial,radial)>b.originRadius.w*b.originRadius.w)continue;
        result+=xyzToRgb(b.powerSigma.xyz)*exp(-b.powerSigma.w*t)*max(dot(n,-b.directionLength.xyz),0)/
                (PI*b.originRadius.w*b.originRadius.w);
    }
    return result;
}
float3 beamRadiance(float3 o,float3 d,float limit,uint medium) {
    if(!Play.x||!Water.z||limit<EPS||(medium!=0&&medium!=8))return 0;
    float3 result=0;
    for(uint candidates=Stats.Load(medium==8?BEAM_WATER_MASK:BEAM_AIR_MASK);candidates;candidates&=candidates-1) {
        uint i=firstbitlow(candidates);Beam b=Beams[i];
        float3 v=o-b.originRadius.xyz,axis=b.directionLength.xyz;
        float vd=dot(v,axis),dd=dot(d,axis),lo=0,hi=limit;
        if(abs(dd)<1e-6){if(vd<0||vd>b.directionLength.w)continue;}
        else {
            float a=-vd/dd,c=(b.directionLength.w-vd)/dd;
            lo=max(lo,min(a,c));hi=min(hi,max(a,c));if(lo>=hi)continue;
        }
        float3 vp=v-axis*vd,dp=d-axis*dd;
        float A=dot(dp,dp),B=dot(vp,dp),C=dot(vp,vp)-b.originRadius.w*b.originRadius.w;
        if(A<1e-8){if(C>0)continue;}
        else {
            float discriminant=B*B-A*C;if(discriminant<=0)continue;
            float root=sqrt(discriminant);lo=max(lo,(-B-root)/A);hi=min(hi,(-B+root)/A);
        }
        if(lo>=hi)continue;
        // Propagation convention: laser travels axis, scattered light travels -d.
        float g=b.medium.y,denom=1+g*g-2*g*dot(axis,-d);
        float phase=(1-g*g)/(4*PI*denom*sqrt(denom));
        float step=(hi-lo)*.25;float3 integral=0;
        [unroll] for(uint q=0;q<4;++q) {
            float t=lo+(q+.5)*step,s=clamp(vd+t*dd,0,b.directionLength.w);
            integral+=exp(-b.powerSigma.w*s)*exp(-extinctionRgb(medium)*t)*step;
        }
        float3 power=xyzToRgb(b.powerSigma.xyz);
        result+=power*(b.medium.x*phase/(PI*b.originRadius.w*b.originRadius.w))*integral;
    }
    return result;
}
