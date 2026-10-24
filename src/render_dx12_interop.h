// Private DirectX 12 interop for the native render backend.
//
// The ImGui debugger's own DX12 renderer (debug_ui/debug_ui_dx12.cpp) shares
// the main window's device, direct command queue and shader-visible SRV heap
// rather than standing up a second device. This header is the only sanctioned
// way to reach into render_dx12.cpp for those objects: it checks that the
// backend handed in really is the native one and still healthy, so a caller
// holding a plain render_backend_t (which may just as well be the SDL backend)
// cannot misinterpret it.
//
// C++ only, and only in HAS_DX12 builds -- COM types are deliberately never
// exposed to the portable C sources, which see the opaque render_backend.h API.
#ifndef X16_RENDER_DX12_INTEROP_H
#define X16_RENDER_DX12_INTEROP_H

#if defined(HAS_DX12) && defined(__cplusplus)

#include "render_backend.h"

#include <d3d12.h>

// True when `backend` is the native DX12 backend and its device has not been
// lost. Everything below returns null / false unless this holds.
bool render_dx12_is_native(render_backend_t *backend);

// Borrowed objects. Valid only for the lifetime of `backend`; never released by
// the caller.
ID3D12Device *render_dx12_device(render_backend_t *backend);
ID3D12CommandQueue *render_dx12_queue(render_backend_t *backend);
ID3D12DescriptorHeap *render_dx12_srv_heap(render_backend_t *backend);

// A slot in the shared shader-visible SRV heap, for a texture the backend does
// not own itself (the ImGui font atlas). Freed slots are recycled.
bool render_dx12_srv_allocate(render_backend_t *backend, unsigned int *slot);
void render_dx12_srv_free(render_backend_t *backend, unsigned int slot);
D3D12_CPU_DESCRIPTOR_HANDLE render_dx12_srv_cpu_handle(render_backend_t *backend,
                                                       unsigned int slot);
D3D12_GPU_DESCRIPTOR_HANDLE render_dx12_srv_gpu_handle(render_backend_t *backend,
                                                       unsigned int slot);

// The GPU descriptor handle of a texture created through
// render_backend_texture_create() on the native backend -- i.e. the value to
// hand ImGui as an ImTextureID. False if the texture is not a native one.
bool render_dx12_texture_gpu_handle(render_texture_t *texture,
                                    D3D12_GPU_DESCRIPTOR_HANDLE *handle);

// Submit whatever the backend has recorded but not yet executed (texture
// uploads in particular). Because the debugger draws on the same queue, doing
// this before its command list is executed is what makes those uploads visible
// to it. False if the backend went unhealthy.
bool render_dx12_flush_pending(render_backend_t *backend);

// Signal and wait on the shared queue, so every command list submitted so far
// -- the debugger's included -- has retired. Required before recycling any
// shared resource (descriptor slots, textures). False if the backend went
// unhealthy; a lost device is reported as success because there is nothing left
// to wait for.
bool render_dx12_wait_idle(render_backend_t *backend);

#endif // HAS_DX12 && __cplusplus

#endif // X16_RENDER_DX12_INTEROP_H
