// Dear ImGui DirectX 12 vertex shader, copied verbatim from the embedded HLSL
// string in ImGui_ImplDX12_CreateDeviceObjects() (imgui_impl_dx12.cpp,
// v1.91.5-docking, commit 97fa363adb1439d11b613d96bcda86d44ea16f4e). Only the
// line-continuation backslashes of the C string literal were removed.
//
// This project compiles it ahead of time into imgui_impl_dx12_vs.h so the
// emulator never calls D3DCompile() and therefore never depends on
// d3dcompiler_47.dll at runtime -- exactly the "preferred solution" upstream
// suggests in the comment above those shader blobs.
//
// Regenerate with the Windows SDK's fxc after any upstream update:
//   fxc /nologo /T vs_5_0 /E main /Fh imgui_impl_dx12_vs.h /Vn g_imgui_dx12_vs_main imgui_impl_dx12_vs.hlsl
// Ordinary builds do not need a shader compiler.

cbuffer vertexBuffer : register(b0)
{
  float4x4 ProjectionMatrix;
};
struct VS_INPUT
{
  float2 pos : POSITION;
  float4 col : COLOR0;
  float2 uv  : TEXCOORD0;
};

struct PS_INPUT
{
  float4 pos : SV_POSITION;
  float4 col : COLOR0;
  float2 uv  : TEXCOORD0;
};

PS_INPUT main(VS_INPUT input)
{
  PS_INPUT output;
  output.pos = mul( ProjectionMatrix, float4(input.pos.xy, 0.f, 1.f));
  output.col = input.col;
  output.uv  = input.uv;
  return output;
}
