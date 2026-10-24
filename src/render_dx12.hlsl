struct Vertex {
    float2 position : POSITION;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

struct PixelInput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

Texture2D image : register(t0);
SamplerState imageSampler : register(s0);

cbuffer DrawConstants : register(b0) {
    uint forceOpaque;
};

PixelInput vs_main(Vertex input)
{
    PixelInput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

float4 ps_main(PixelInput input) : SV_TARGET
{
    float4 sampled = image.Sample(imageSampler, input.uv);
    float alpha = forceOpaque != 0 ? 1.0 : sampled.a;
    return float4(sampled.rgb * input.color.rgb, alpha * input.color.a);
}
