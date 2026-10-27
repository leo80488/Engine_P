// Spot shadow VS: Shadow.vs.hlsl plus the static / dynamic instance filter
// (_svShadowFilter); the other shadow families compile it without the filter.
#define SPOT_MIXED_SHADOW 1
#include "Shadow.vs.hlsl"
