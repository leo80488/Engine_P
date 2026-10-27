#ifndef RESTIR_PAIRWISE_HLSLI
#define RESTIR_PAIRWISE_HLSLI
// Algorithm reference: the pinned NVIDIA RTXDI-Library (shaders/Rtxdi/VERSION.json),
// DI/PairwiseStreaming.hlsli and Utils/Math.hlsli. Engine-local balance heuristic: the K
// neighbours form the alternative technique, so their population is K*Mi. Keeps the
// engine's attempted-pair normalization and the canonical fallback for rejected pairs;
// docs/design/RTXDI_DI_Reference_Integration.md lists the deviations.
float RTPairNeighbour(float Mi, float piYi, float Mc, float pcShiftYi, float J, uint K, bool reversible)
{
    float a=K*Mi*piYi, b=Mc*pcShiftYi*abs(J);
    return reversible && K>0 && a+b>0 ? a/(a+b)/K : 0;
}
float RTPairCanonical(float Mc, float pcYc, float Mi, float piInverseYc, float inverseJ, uint K, bool reversible)
{
    if(K==0) return 0;
    if(!reversible) return 1.0/K;
    float a=Mc*pcYc, b=K*Mi*piInverseYc*abs(inverseJ);
    return a+b>0 ? a/(a+b)/K : 1.0/K;
}
#endif
