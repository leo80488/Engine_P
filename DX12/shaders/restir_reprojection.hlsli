#ifndef RESTIR_REPROJECTION_HLSLI
#define RESTIR_REPROJECTION_HLSLI
// Motion is previous-minus-current UV without jitter. Histories store samples
// at full-resolution positions (traceCoordinate * stride + 0.5), including at
// odd viewport dimensions. Never substitute traceExtent/fullExtent for stride.
float RTPreviousTraceCoordinate(float traceCoordinate,float motionUV,float fullExtent,float stride,float jitterDelta)
{ return traceCoordinate+(motionUV*fullExtent+jitterDelta)/stride; }
float RTTraceCoordinateFromFullUV(float uv,float fullExtent,float stride)
{ return (uv*fullExtent-.5)/stride; }
#endif
