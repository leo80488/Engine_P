// Shared GPU layouts and parallel-transport helpers for the procedural-tube beam system.
// Mirrors include/Graphics/BeamSystem.h.

#ifndef BEAM_COMMON_HLSLI
#define BEAM_COMMON_HLSLI

// Tube topology - must match C++ kRadialSegs / kAxialSegs / kMaxControlPointsPerBeam.
#define BEAM_RADIAL_SEGS                 12u
#define BEAM_AXIAL_SEGS                  16u
#define BEAM_RING_VERT_COUNT             (BEAM_RADIAL_SEGS + 1u)   // +1 for UV seam
#define BEAM_AXIAL_RING_COUNT            (BEAM_AXIAL_SEGS + 1u)
#define BEAM_VERTS_PER_BEAM              (BEAM_RING_VERT_COUNT * BEAM_AXIAL_RING_COUNT)
#define BEAM_INDICES_PER_BEAM            (BEAM_RADIAL_SEGS * BEAM_AXIAL_SEGS * 6u)
#define BEAM_MAX_CONTROL_POINTS_PER_BEAM 8u

// 32 bytes - must match C++ BeamControlPointGPU.
struct BeamControlPoint
{
    float3 position;
    float  radius;
    float4 colorTint;    // linear HDR; .a = intensity along the beam
};

// 256 bytes, one per-beam compute CBV slot: the CS reads where to write its slice of the
// shared VBs and which control-point window to read. Pads fill the slot to 256.
struct BeamGenParams
{
    uint  controlPointOffset;   // index into gControlPoints[]
    uint  controlPointCount;    // typically 2..8
    uint  vertexBaseElement;    // first vertex in the shared VBs
    uint  _pad0;
    float globalRadiusScale;    // multiplies each control point's radius
    float wobbleAmplitude;      // perpendicular noise
    float wobbleSpeed;          // frequency
    float time;                 // global time for the wobble noise
    float4 _pad1;
    float4 _pad2;
    float4 _pad3;
    float4 _pad4;
    float4 _pad5;
    float4 _pad6;
    float4 _pad7;
    float4 _pad8;
    float4 _pad9;
    float4 _pad10;
    float4 _pad11;
    float4 _pad12;
    float4 _pad13;
    float4 _pad14;
};

// Reference axis not parallel to t, used to seed the parallel-transport frame.
float3 BeamReferenceVector(float3 t)
{
    return abs(t.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
}

#endif // BEAM_COMMON_HLSLI
