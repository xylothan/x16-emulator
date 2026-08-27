// Dear ImGui DirectX 12 pixel shader, copied verbatim from the embedded HLSL
// string in ImGui_ImplDX12_CreateDeviceObjects() (imgui_impl_dx12.cpp,
// v1.91.5-docking, commit 97fa363adb1439d11b613d96bcda86d44ea16f4e). Only the
// line-continuation backslashes of the C string literal were removed.
//
// This project compiles it ahead of time into imgui_impl_dx12_ps.h so the
// emulator never calls D3DCompile() and therefore never depends on
// d3dcompiler_47.dll at runtime -- exactly the "preferred solution" upstream
// suggests in the comment above those shader blobs.
//
// Regenerate with the Windows SDK's fxc after any upstream update:
//   fxc /nologo /T ps_5_0 /E main /Fh imgui_impl_dx12_ps.h /Vn g_imgui_dx12_ps_main imgui_impl_dx12_ps.hlsl
// Ordinary builds do not need a shader compiler.

struct PS_INPUT
{
  float4 pos : SV_POSITION;
  float4 col : COLOR0;
  float2 uv  : TEXCOORD0;
};
SamplerState sampler0 : register(s0);
Texture2D texture0 : register(t0);

float4 main(PS_INPUT input) : SV_Target
{
  float4 out_col = input.col * texture0.Sample(sampler0, input.uv);
  return out_col;
}
