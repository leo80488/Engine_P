// Engine-default particle sprite PS: the engine-owned surface template with the
// weak-default particle surface hook (radial falloff + visual mode + optional
// bindless texture), all in particle_surface.hlsli and shared with every custom
// particle pixel shader. A custom particle PS replaces only the
// EvaluateParticleSurface hook and reuses the same main() + composite/discard.

#include "particle_surface.hlsli"
