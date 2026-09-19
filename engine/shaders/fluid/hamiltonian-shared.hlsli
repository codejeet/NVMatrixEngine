#ifndef HAMILTONIAN_SHARED
#define HAMILTONIAN_SHARED
cbuffer HamiltonianConstants : register(b3) {
    float4 WaveDomain; // min x,z, length x,z (physical reflecting rectangle)
    float4 WavePhysics; // depth, mean height, g, epsilon
    float4 WaveCoupling; // sigma / s, FAB width / m, dt / s, initial amplitude / m
    float4 WaveLocal; // min x,z, max x,z of the 3D embedding box
    uint4 WaveGrid; // physical resolution, doubled FFT resolution, depth layers, HOS order
    float4 WaveFill; // horizontal/vertical transfer correction rates; pending mean rise/local volume on CPU
    float4 WaveMass; // particle volume, CPU snapshot of received particles and local transfers
    float4 WaveSpectrum; // wind speed, optical horizon continuation, minimum initial wavelength, reserved
};
#ifndef WAVE_DATA_REGISTER
#define WAVE_DATA_REGISTER u38
#endif
RWStructuredBuffer<float4> WaveData : register(WAVE_DATA_REGISTER);
float4 wavePlane(float2 xz,uint plane) {
    uint n=WaveGrid.x;float2 p=(xz-WaveDomain.xy)/WaveDomain.zw*n-.5;
    p=clamp(p,0,float(n-1));uint2 a=uint2(p),b=min(a+1,n-1);float2 f=frac(p);
    uint base=plane*n*n;
    return lerp(lerp(WaveData[base+a.y*n+a.x],WaveData[base+a.y*n+b.x],f.x),
                lerp(WaveData[base+b.y*n+a.x],WaveData[base+b.y*n+b.x],f.x),f.y);
}
float waveEdge(float2 xz) {
    if(WaveMass.w!=0)return wavePlane(xz,WaveGrid.z+1).x;
    float2 q=min(xz-WaveLocal.xy,WaveLocal.zw-xz);
    return min(q.x,q.y);
}
float wavePreviousEdge(float2 xz) {
    return WaveMass.w!=0?wavePlane(xz,WaveGrid.z+1).y:waveEdge(xz);
}
float waveInterior(float2 xz) {
    // A four-cell prescribed reservoir surrounds each activity region, then
    // a smooth overlap connects full 3D water to the canonical wave surface.
    return smoothstep(WaveCoupling.y,2*WaveCoupling.y,waveEdge(xz));
}
bool wavePressureCell(int3 c,float4 minimumCell) {
    return waveEdge(minimumCell.xz+(float2(c.x,c.z)+.5)*minimumCell.w)>=WaveCoupling.y;
}
float3 waveVelocity(float3 p) {
    float layer=saturate((p.y-(WavePhysics.y-WavePhysics.x))/WavePhysics.x)*(WaveGrid.z-1);
    uint a=uint(layer),b=min(a+1,WaveGrid.z-1);
    return lerp(wavePlane(p.xz,1+a).xyz,wavePlane(p.xz,1+b).xyz,frac(layer));
}
float3 waveBoundaryVelocity(float3 p) {
    // Match the pressure region to measured volume transfers. The horizontal
    // correction accounts for mass already delivered as local particles;
    // the vertical correction tracks mean rise, with zero flow at the floor.
    float y=max(0,p.y-(WavePhysics.y-WavePhysics.x));
    if(WaveMass.w!=0) {
        float2 h=WaveDomain.zw/WaveGrid.x;
        float2 n=float2((waveEdge(p.xz+float2(h.x,0))-waveEdge(p.xz-float2(h.x,0)))/h.x,
                        (waveEdge(p.xz+float2(0,h.y))-waveEdge(p.xz-float2(0,h.y)))/h.y);
        n*=rsqrt(max(dot(n,n),1e-8));
        return waveVelocity(p)+float3(n.x*WaveFill.x,WaveFill.y*y,n.y*WaveFill.x);
    }
    float2 lo=WaveLocal.xy,hi=WaveLocal.zw;
    float2 left=float2(lo>WaveDomain.xy+1e-4),right=float2(hi<WaveDomain.xy+WaveDomain.zw-1e-4);
    float2 open=min(left+right,1);
    float2 share=open/max(open.x+open.y,1);
    float2 inflow=share*(-(p.xz-lo)+left*(1-.5*right)*(hi-lo));
    return waveVelocity(p)+float3(WaveFill.x*inflow.x,WaveFill.y*y,WaveFill.x*inflow.y);
}
#endif
