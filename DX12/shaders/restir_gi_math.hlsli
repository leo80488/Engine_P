#ifndef RESTIR_GI_MATH_HLSLI
#define RESTIR_GI_MATH_HLSLI
// Solid-angle Jacobian for reconnecting the SAME secondary surface point.
// Receiver cosine belongs to the integrand, not this area/solid-angle ratio.
float GIReconnectionJacobian(float oldDistance2,float newDistance2,float oldHitCos,float newHitCos)
{
    if(oldDistance2<=1e-8 || newDistance2<=1e-8 || oldHitCos<=1e-8 || newHitCos<=0) return 0;
    return newHitCos*oldDistance2/(oldHitCos*newDistance2);
}
#endif
