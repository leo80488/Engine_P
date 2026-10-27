// Outputs (instanceSlot + 1) to a R32_UINT render target.
// Value 0 is reserved for "no object" (background clear value).

struct PSIn
{
    float4               sv           : SV_Position;
    nointerpolation uint instanceSlot : TEXCOORD0;
};

uint main(PSIn input) : SV_Target
{
    return input.instanceSlot + 1u;
}
