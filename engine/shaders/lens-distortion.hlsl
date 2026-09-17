#include "lens-mapping.hlsli"
RWTexture2D<float4> BidirectionalDistortion : register(u0);
cbuffer LensConstants : register(b0) { float2 OutputSize; float Fisheye; float HalfDiagonal; };
[numthreads(8,8,1)]
void FGDistortion(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    BidirectionalDistortion.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    float2 uv = (id.xy + .5) / float2(width, height);
    float aspect = OutputSize.x / OutputSize.y;
    // DLSS expects signed UV displacements, not pixel positions or warped
    // motion vectors. Inverse correspondences may legitimately leave the image.
    float2 undistorted = fisheyeToRectilinearUv(uv, aspect, HalfDiagonal);
    float2 distorted = rectilinearToFisheyeUv(uv, aspect, HalfDiagonal);
    // Round explicitly to the nearest FP16 lattice point before the typed UAV
    // store (and legacy f32tof16), whose conversion truncates toward zero.
    // Clamp the exponent at the FP16 normal minimum to handle subnormals/zero.
    float4 displacement = float4(undistorted - uv, distorted - uv);
    uint4 exponent = max((asuint(abs(displacement)) >> 23) & 255u, 113u);
    float4 spacing = asfloat((exponent - 10u) << 23);
    BidirectionalDistortion[id.xy] = round(displacement / spacing) * spacing;
}
