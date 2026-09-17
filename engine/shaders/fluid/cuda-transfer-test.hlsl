#include "common.hlsli"
// Only a fixture: perturb projected velocity while retaining the old velocity
// to exercise the FLIP delta, not merely a PIC round trip.
[numthreads(128,1,1)]
void PerturbFaces(uint id : SV_DispatchThreadID) {
    if(id>=faceStride()*3)return;
    Faces[id].x += float(int(id%13)-6)*.013;
}
