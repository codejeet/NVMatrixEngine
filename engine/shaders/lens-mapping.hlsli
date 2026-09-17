#ifndef LAB_LENS_MAPPING
#define LAB_LENS_MAPPING
// Shared by picking, final presentation and DLSS's bidirectional field.
// Radius is normalized by the image's half diagonal; halfDiagonal is radians.
#ifdef __cplusplus
inline
#endif
float fisheyeToRectilinearScale(float radius, float halfDiagonal) {
    float sine = sin(halfDiagonal * .5f), tangent = tan(halfDiagonal);
    if (radius < 1e-7f)
        return 2 * sine / tangent;
    float argument = radius * sine;
    if (argument > .99999f)
        argument = .99999f;
    return tan(2 * asin(argument)) / (tangent * radius);
}
#ifdef __cplusplus
inline
#endif
float rectilinearToFisheyeScale(float radius, float halfDiagonal) {
    float sine = sin(halfDiagonal * .5f), tangent = tan(halfDiagonal);
    if (radius < 1e-7f)
        return tangent / (2 * sine);
    return sin(.5f * atan(radius * tangent)) / (sine * radius);
}
#ifndef __cplusplus
float2 fisheyeToRectilinearUv(float2 uv, float aspect, float halfDiagonal) {
    float2 q = uv * 2 - 1;
    float radius = length(q * float2(aspect, 1)) / sqrt(1 + aspect * aspect);
    return .5 + .5 * q * fisheyeToRectilinearScale(radius, halfDiagonal);
}
float2 rectilinearToFisheyeUv(float2 uv, float aspect, float halfDiagonal) {
    float2 q = uv * 2 - 1;
    float radius = length(q * float2(aspect, 1)) / sqrt(1 + aspect * aspect);
    return .5 + .5 * q * rectilinearToFisheyeScale(radius, halfDiagonal);
}
#endif
#endif
