#ifndef FLUID_MAC_ROW
#define FLUID_MAC_ROW
// Geometry-dependent boundary conductance and control volume are explicit.
// A hierarchy must not infer unit air weights or full cells from topology.
struct MacRow {uint count; float diagonal,rhs,step; uint neighbor[24]; float coefficient[24];float boundaryDiagonal,volumeUnits;};
#endif
