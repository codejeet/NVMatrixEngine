cbuffer Test : register(b0) { uint count, allocated, mode, seed; }
RWStructuredBuffer<uint> Data : register(u0);
[numthreads(128, 1, 1)]
void InteropTest(uint3 tid : SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= allocated) return;
    if (mode == 0) Data[i] = i < count ? (i * 1664525u) ^ seed : 0xcafebabeu;
    else if (i < count) Data[i] = (Data[i] * 3u) ^ (seed + i);
}
