// Reversible RGB encoding around RR; alpha and reflectance guides stay unchanged.
cbuffer Params : register(b0)
{
    uint width,height,inputIndex,outputIndex;
    float scale;
};
[numthreads(8,8,1)]
void main(uint3 tid:SV_DispatchThreadID)
{
    if(tid.x>=width || tid.y>=height) return;
    RWTexture2D<float4> output=ResourceDescriptorHeap[outputIndex];
    float4 value;
    if(inputIndex==0xffffffffu) value=output[tid.xy];
    else
    {
        Texture2D<float4> input=ResourceDescriptorHeap[inputIndex];
        value=input.Load(int3(tid.xy,0));
    }
    output[tid.xy]=float4(value.rgb*scale,value.a);
}
