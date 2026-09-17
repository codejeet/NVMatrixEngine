#ifndef FLUID_SPACETIME_SHARED
#define FLUID_SPACETIME_SHARED
// Braun et al. 2026, equations 8, 11, 19. Positive residual means the
// particle sample lags global time. Keep seconds, not a normalized phase:
// changing dt must not discard outstanding particle time.
cbuffer SpacetimeFrame : register(b3) {
    float4 Spacetime; // previous dt, current dt, jitter strength, phase eta
    uint4 SpacetimeControl; // seed, simulation step (not render frame), first, count
};
RWStructuredBuffer<float> ParticleTime : register(u38);
float temporalKernel(float tau) {
    if(tau > .5 || tau < -.5)return 0;
    float r=tau-.5,q=max(0,1-r*r);
    return (35.0/16.0)*q*q*q;
}
float temporalParticleWeight(uint id) {
    return temporalKernel(-ParticleTime[id]/Spacetime.x);
}
uint spacetimeHash(uint x) {
    x^=x>>16;x*=0x7feb352d;x^=x>>15;x*=0x846ca68b;return x^(x>>16);
}
float2 particleTimeAdvance(uint id,float residual,float speed) {
    float dt=Spacetime.y;
    if(dt==0)return float2(0,residual); // pause never consumes residual or RNG
    float cfl=saturate(speed*dt/DomainMinCell.w);
    float gamma=Spacetime.z*cfl*cfl*(3-2*cfl);
    uint bits=spacetimeHash(id^spacetimeHash(SpacetimeControl.x)^spacetimeHash(SpacetimeControl.y));
    float jitter=((bits>>8)*(1.0/16777216.0)-.5)*gamma*dt;
    float wanted=dt+residual;
    float actual=clamp(wanted+jitter,0,2*dt);
    return float2(actual,wanted-actual);
}
#endif
