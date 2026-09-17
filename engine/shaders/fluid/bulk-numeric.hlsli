#ifndef FLUID_BULK_NUMERIC
#define FLUID_BULK_NUMERIC
// Only the opt-in coupled pressure/transport path uses FP64 inventories.
// Fine geometry and APIC remain FP32. Their restricted capacities must be
// summed without another FP32 rounding so swept-volume conservation agrees
// with the fine-face pressure equations, including almost-closed cut cells.
#ifndef BULK_PRECISE
#define BULK_PRECISE 0
#endif
#if BULK_PRECISE
#define BulkScalar double
#define Bulk2 double2
#define Bulk3 double3
#define Bulk4 double4
#else
#define BulkScalar float
#define Bulk2 float2
#define Bulk3 float3
#define Bulk4 float4
#endif
#endif
