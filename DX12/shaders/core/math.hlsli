// core/math.hlsli - the engine's shader constants. The rules every core header follows are at
// the top of core/gbuffer_codec.hlsli.
// PI is the one name: do not define or alias another in engine shaders (a second
// `static const float PI` in a translation unit that includes this is a redefinition). The
// vendored names (RTXDI_PI, XeGTAO's and NRD's) are not ours to edit.

#ifndef CORE_MATH_HLSLI
#define CORE_MATH_HLSLI

static const float PI = 3.14159265358979;

#endif // CORE_MATH_HLSLI
