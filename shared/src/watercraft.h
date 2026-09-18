#pragma once
#include <DirectXMath.h>
#include <array>
#include <algorithm>
namespace watercraft {
// Shared watertight hull for DXR, Bullet and the fluid SDF. Metres; bow is -Z.
inline constexpr float keel = -.45f, deck = .30f, stern = 2.7f, bow = -3.1f;
inline constexpr float mass = 550, riderMass = 60, ballRadius = .68f, glassDensity = 2500;
inline constexpr float thrust = 6000, boardingDistance = 4.5f;
inline constexpr unsigned stations = 8;
inline constexpr float stationLength = (stern-bow)/stations;
inline float halfBeam(float z, float y) {
    return std::max(0.f, std::min(1.09f+.7f*y, 1.6f+.55f*z+.35f*y));
}
inline std::array<DirectX::XMFLOAT3,10> hullVertices() {
    std::array<DirectX::XMFLOAT3,10> v;
    for (int layer=0;layer<2;++layer) {
        float y=layer?deck:keel, width=1.09f+.7f*y;
        float shoulder=(width-1.6f-.35f*y)/.55f, nose=-(1.6f+.35f*y)/.55f;
        v[layer*5+0]={-width,y,stern}; v[layer*5+1]={width,y,stern};
        v[layer*5+2]={width,y,shoulder}; v[layer*5+3]={0,y,nose};
        v[layer*5+4]={-width,y,shoulder};
    }
    return v;
}
inline std::array<DirectX::XMFLOAT3,48> hullTriangles() {
    const auto v=hullVertices(); std::array<DirectX::XMFLOAT3,48> triangles; unsigned n=0;
    auto tri=[&](unsigned a,unsigned b,unsigned c){triangles[n++]=v[a];triangles[n++]=v[b];triangles[n++]=v[c];};
    for(unsigned i=1;i<4;++i){tri(0,i+1,i);tri(5,5+i,6+i);}
    for(unsigned i=0;i<5;++i){unsigned j=(i+1)%5;tri(i,j,j+5);tri(i,j+5,i+5);}
    return triangles;
}
// One side of a longitudinal quadrature strip. Integrate the actual flared
// and tapered cross-section rather than retaining pontoon displacement.
inline float columnVolume(float z,float waterY) {
    float lo=std::max(keel, std::max(-1.09f/.7f,-(1.6f+.55f*z)/.35f));
    float hi=std::clamp(waterY,keel,deck);
    if(hi<=lo)return 0;
    float split=std::clamp((.51f+.55f*z)/.35f,lo,hi);
    return stationLength*(1.09f*(split-lo)+.35f*(split*split-lo*lo)+
        (1.6f+.55f*z)*(hi-split)+.175f*(hi*hi-split*split));
}
inline float displacedVolume(float waterY) {
    float volume=0;
    for(unsigned i=0;i<stations;++i)volume+=2*columnVolume(bow+(i+.5f)*stationLength,waterY);
    return volume;
}
inline float immersedFraction(float height, float center, float halfHeight) {
    return std::clamp((height - center + halfHeight) / (2 * halfHeight), 0.f, 1.f);
}
inline float sphereVolume(float radius, float height) {
    const float h = std::clamp(height, 0.f, 2 * radius);
    return 3.14159265359f * h * h * (radius - h / 3);
}
inline float ballMass(bool floating) {
    // Float keeps the lightweight hollow avatar; Sink fills the same volume
    // with glass. Optical appearance and displaced volume do not change.
    return floating ? riderMass : glassDensity * sphereVolume(ballRadius, 2 * ballRadius);
}
} // namespace watercraft
