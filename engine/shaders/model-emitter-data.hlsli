#ifndef LAB_MODEL_EMITTER_DATA
#define LAB_MODEL_EMITTER_DATA
struct ModelEmitter { float4 pArea,e1Cdf,e2Pdf; uint a,b,c,material; };
StructuredBuffer<ModelEmitter> ModelLights : register(t12);
#endif
