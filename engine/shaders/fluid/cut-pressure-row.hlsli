#ifndef FLUID_CUT_PRESSURE_ROW
#define FLUID_CUT_PRESSURE_ROW
// Only the opt-in cut projection allocates this precision sidecar. The ordinary
// MAC operator and multigrid preconditioner retain their compact FP32 rows.
// Six face conductances suffice: cut cells are guarded from 2:1 junctions.
// diagonal < 0 marks a regular mixed-grid row, whose original stencil is used.
struct CutPressureRow {double rhs,diagonal;double conductance[6];};
#endif
