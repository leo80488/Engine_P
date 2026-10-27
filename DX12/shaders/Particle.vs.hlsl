// Engine-default particle billboard VS: the engine-owned billboard template with
// the weak-default vertex hook (identity, no vertex animation). Quad expansion,
// dead-particle culling and clip-space projection live in particle_vertex.hlsli,
// shared with every custom particle vertex shader. A custom particle VS replaces
// only the CustomizeParticleVertex hook and reuses the same main(), so it can never
// desync the VS -> PS varyings.

#include "particle_vertex.hlsli"
