
struct PushConstants
{
    uint ResourceDescriptorIndex;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants : register(b3, space4);

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    return texture.SampleLevel(g_SamplerDescriptorHeap[0], texCoord, 0.0);
}
