#include "render_backend_internal.h"

#include <SDL.h>
#include <SDL_syswm.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <math.h>
#include <new>
#include <stdio.h>
#include <string.h>

#include "render_dx12_vs.h"
#include "render_dx12_ps.h"
#include "render_dx12_interop.h"

namespace {

constexpr UINT kFrameCount = 3;
constexpr UINT kMaxTextures = 256;
constexpr UINT kVertexUploadSize = 4 * 1024 * 1024;
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

struct Vertex {
	float x;
	float y;
	float u;
	float v;
	float r;
	float g;
	float b;
	float a;
};

struct Dx12Frame {
	ID3D12CommandAllocator *allocator;
	ID3D12Resource *vertices;
	uint8_t *mappedVertices;
	UINT verticesUsed;
	UINT64 fenceValue;
	UINT64 serial;
};

struct Dx12Texture {
	render_texture_t base;
	ID3D12Resource *resource;
	ID3D12Resource *uploads[kFrameCount];
	uint8_t *mapped[kFrameCount];
	UINT uploadPitch;
	UINT width;
	UINT height;
	UINT descriptor;
	bool hasDescriptor;
	UINT uploadFrame;
	UINT64 uploadSerial;
	render_pixel_format_t sourceFormat;
	DXGI_FORMAT dxgiFormat;
	render_scale_quality_t quality;
	render_color_t color;
	D3D12_RESOURCE_STATES state;
	bool opaque;
};

struct Dx12Backend {
	render_backend_t base;
	HWND window;
	IDXGIFactory4 *factory;
	ID3D12Device *device;
	ID3D12CommandQueue *queue;
	IDXGISwapChain3 *swapChain;
	ID3D12DescriptorHeap *rtvHeap;
	ID3D12DescriptorHeap *srvHeap;
	ID3D12DescriptorHeap *samplerHeap;
	ID3D12RootSignature *rootSignature;
	ID3D12PipelineState *pipeline;
	ID3D12GraphicsCommandList *commandList;
	ID3D12Fence *fence;
	HANDLE fenceEvent;
	ID3D12Resource *backBuffers[kFrameCount];
	Dx12Frame frames[kFrameCount];
	Dx12Texture *whiteTexture;
	UINT rtvStride;
	UINT srvStride;
	UINT samplerStride;
	UINT nextDescriptor;
	UINT freeDescriptors[kMaxTextures];
	UINT freeDescriptorCount;
	UINT width;
	UINT height;
	int logicalWidth;
	int logicalHeight;
	UINT activeFrame;
	UINT64 nextFenceValue;
	bool tearing;
	bool commandListOpen;
	bool backBufferIsRenderTarget;
	bool healthy;
	D3D12_VIEWPORT viewport;
	D3D12_RECT scissor;
};

template <typename T>
void
release(T *&object)
{
	if (object) {
		object->Release();
		object = NULL;
	}
}

void
log_hresult(const char *operation, HRESULT result)
{
	fprintf(stderr, "DX12: %s failed (0x%08lX).\n", operation,
	        static_cast<unsigned long>(result));
}

void
mark_device_lost(Dx12Backend *backend, const char *operation, HRESULT result)
{
	log_hresult(operation, result);
	if (backend->device) {
		log_hresult("GetDeviceRemovedReason", backend->device->GetDeviceRemovedReason());
	}
	backend->healthy = false;
}

UINT
align_up(UINT value, UINT alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

D3D12_HEAP_PROPERTIES
heap_properties(D3D12_HEAP_TYPE type)
{
	D3D12_HEAP_PROPERTIES result = {};
	result.Type = type;
	result.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	result.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	result.CreationNodeMask = 1;
	result.VisibleNodeMask = 1;
	return result;
}

D3D12_RESOURCE_DESC
buffer_desc(UINT64 size)
{
	D3D12_RESOURCE_DESC result = {};
	result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	result.Width = size;
	result.Height = 1;
	result.DepthOrArraySize = 1;
	result.MipLevels = 1;
	result.SampleDesc.Count = 1;
	result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	return result;
}

D3D12_RESOURCE_DESC
texture_desc(UINT width, UINT height, DXGI_FORMAT format)
{
	D3D12_RESOURCE_DESC result = {};
	result.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	result.Width = width;
	result.Height = height;
	result.DepthOrArraySize = 1;
	result.MipLevels = 1;
	result.Format = format;
	result.SampleDesc.Count = 1;
	result.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	return result;
}

// The DXGI format a source pixel format is expanded into on upload. The 4-bit
// VERA format is widened to BGRA on the CPU, so it shares BGRA's texture.
DXGI_FORMAT
dxgi_format_for(render_pixel_format_t format)
{
	return format == RENDER_PIXEL_FORMAT_RGBA8888 ? DXGI_FORMAT_R8G8B8A8_UNORM
	                                              : DXGI_FORMAT_B8G8R8A8_UNORM;
}

void
transition(ID3D12GraphicsCommandList *commandList, ID3D12Resource *resource,
           D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
	if (before == after) {
		return;
	}
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	commandList->ResourceBarrier(1, &barrier);
}

bool
wait_for_fence(Dx12Backend *backend, UINT64 value)
{
	if (!value || backend->fence->GetCompletedValue() >= value) {
		return true;
	}
	HRESULT result = backend->fence->SetEventOnCompletion(value, backend->fenceEvent);
	if (FAILED(result)) {
		mark_device_lost(backend, "ID3D12Fence::SetEventOnCompletion", result);
		return false;
	}
	if (WaitForSingleObject(backend->fenceEvent, INFINITE) != WAIT_OBJECT_0) {
		fprintf(stderr, "DX12: waiting for the GPU fence failed.\n");
		backend->healthy = false;
		return false;
	}
	return true;
}

bool
wait_for_gpu(Dx12Backend *backend)
{
	if (!backend->queue || !backend->fence) {
		return true;
	}
	const UINT64 value = backend->nextFenceValue++;
	HRESULT result = backend->queue->Signal(backend->fence, value);
	if (FAILED(result)) {
		mark_device_lost(backend, "ID3D12CommandQueue::Signal", result);
		return false;
	}
	return wait_for_fence(backend, value);
}

bool
begin_commands(Dx12Backend *backend)
{
	if (!backend->healthy) {
		return false;
	}
	if (backend->commandListOpen) {
		return true;
	}

	backend->activeFrame = backend->swapChain->GetCurrentBackBufferIndex();
	Dx12Frame *frame = &backend->frames[backend->activeFrame];
	if (!wait_for_fence(backend, frame->fenceValue)) {
		return false;
	}
	HRESULT result = frame->allocator->Reset();
	if (FAILED(result)) {
		mark_device_lost(backend, "ID3D12CommandAllocator::Reset", result);
		return false;
	}
	result = backend->commandList->Reset(frame->allocator, NULL);
	if (FAILED(result)) {
		mark_device_lost(backend, "ID3D12GraphicsCommandList::Reset", result);
		return false;
	}
	frame->verticesUsed = 0;
	++frame->serial;
	backend->commandListOpen = true;
	backend->backBufferIsRenderTarget = false;
	return true;
}

bool
finish_commands(Dx12Backend *backend)
{
	if (!backend->commandListOpen) {
		return backend->healthy;
	}
	if (backend->backBufferIsRenderTarget) {
		transition(backend->commandList, backend->backBuffers[backend->activeFrame],
		           D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
		backend->backBufferIsRenderTarget = false;
	}
	HRESULT result = backend->commandList->Close();
	if (FAILED(result)) {
		mark_device_lost(backend, "ID3D12GraphicsCommandList::Close", result);
		return false;
	}
	ID3D12CommandList *lists[] = { backend->commandList };
	backend->queue->ExecuteCommandLists(1, lists);
	const UINT64 fenceValue = backend->nextFenceValue++;
	result = backend->queue->Signal(backend->fence, fenceValue);
	if (FAILED(result)) {
		mark_device_lost(backend, "ID3D12CommandQueue::Signal", result);
		return false;
	}
	backend->frames[backend->activeFrame].fenceValue = fenceValue;
	backend->commandListOpen = false;
	return true;
}

void
wait_for_idle_if_available(Dx12Backend *backend)
{
	if (!backend->device || FAILED(backend->device->GetDeviceRemovedReason())) {
		return;
	}
	if (backend->commandListOpen && !finish_commands(backend)) {
		return;
	}
	wait_for_gpu(backend);
}

bool
set_render_target(Dx12Backend *backend)
{
	if (!begin_commands(backend)) {
		return false;
	}
	if (!backend->backBufferIsRenderTarget) {
		transition(backend->commandList, backend->backBuffers[backend->activeFrame],
		           D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
		backend->backBufferIsRenderTarget = true;
	}
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = backend->rtvHeap->GetCPUDescriptorHandleForHeapStart();
	rtv.ptr += static_cast<SIZE_T>(backend->activeFrame) * backend->rtvStride;
	backend->commandList->OMSetRenderTargets(1, &rtv, FALSE, NULL);
	backend->commandList->RSSetViewports(1, &backend->viewport);
	backend->commandList->RSSetScissorRects(1, &backend->scissor);
	return true;
}

bool
allocate_descriptor(Dx12Backend *backend, UINT *descriptor)
{
	if (backend->freeDescriptorCount) {
		*descriptor = backend->freeDescriptors[--backend->freeDescriptorCount];
		return true;
	}
	if (backend->nextDescriptor >= kMaxTextures) {
		fprintf(stderr, "DX12: texture descriptor heap is full.\n");
		return false;
	}
	*descriptor = backend->nextDescriptor++;
	return true;
}

void
free_descriptor(Dx12Backend *backend, UINT descriptor)
{
	if (backend->freeDescriptorCount < kMaxTextures) {
		backend->freeDescriptors[backend->freeDescriptorCount++] = descriptor;
	}
}

D3D12_CPU_DESCRIPTOR_HANDLE
srv_cpu_handle(Dx12Backend *backend, UINT descriptor)
{
	D3D12_CPU_DESCRIPTOR_HANDLE handle = backend->srvHeap->GetCPUDescriptorHandleForHeapStart();
	handle.ptr += static_cast<SIZE_T>(descriptor) * backend->srvStride;
	return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE
srv_gpu_handle(Dx12Backend *backend, UINT descriptor)
{
	D3D12_GPU_DESCRIPTOR_HANDLE handle = backend->srvHeap->GetGPUDescriptorHandleForHeapStart();
	handle.ptr += static_cast<UINT64>(descriptor) * backend->srvStride;
	return handle;
}

void
release_texture(Dx12Backend *backend, Dx12Texture *texture)
{
	if (!texture) {
		return;
	}
	for (UINT i = 0; i < kFrameCount; ++i) {
		if (texture->uploads[i] && texture->mapped[i]) {
			texture->uploads[i]->Unmap(0, NULL);
		}
		release(texture->uploads[i]);
	}
	release(texture->resource);
	if (texture->hasDescriptor) {
		free_descriptor(backend, texture->descriptor);
	}
	delete texture;
}

bool
create_texture_resources(Dx12Backend *backend, Dx12Texture *texture)
{
	D3D12_HEAP_PROPERTIES defaultHeap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
	D3D12_RESOURCE_DESC textureDescription =
		texture_desc(texture->width, texture->height, texture->dxgiFormat);
	HRESULT result = backend->device->CreateCommittedResource(
		&defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDescription, D3D12_RESOURCE_STATE_COPY_DEST,
		NULL, IID_PPV_ARGS(&texture->resource));
	if (FAILED(result)) {
		log_hresult("creating texture", result);
		return false;
	}

	texture->uploadPitch = align_up(texture->width * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
	const UINT64 uploadSize = static_cast<UINT64>(texture->uploadPitch) * texture->height;
	D3D12_HEAP_PROPERTIES uploadHeap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
	D3D12_RESOURCE_DESC uploadDescription = buffer_desc(uploadSize);
	for (UINT i = 0; i < kFrameCount; ++i) {
		result = backend->device->CreateCommittedResource(
			&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDescription,
			D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
			IID_PPV_ARGS(&texture->uploads[i]));
		if (FAILED(result)) {
			log_hresult("creating texture upload buffer", result);
			return false;
		}
		D3D12_RANGE readRange = { 0, 0 };
		result = texture->uploads[i]->Map(0, &readRange,
		                                  reinterpret_cast<void **>(&texture->mapped[i]));
		if (FAILED(result)) {
			log_hresult("mapping texture upload buffer", result);
			return false;
		}
	}

	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Format = texture->dxgiFormat;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Texture2D.MipLevels = 1;
	backend->device->CreateShaderResourceView(texture->resource, &srv,
	                                           srv_cpu_handle(backend, texture->descriptor));
	return true;
}

bool
update_texture(Dx12Texture *texture, const render_rect_t *requested,
               const void *pixels, int pitch)
{
	Dx12Backend *backend = reinterpret_cast<Dx12Backend *>(texture->base.backend);
	if (!pixels || pitch <= 0 || !begin_commands(backend)) {
		return false;
	}
	Dx12Frame *frame = &backend->frames[backend->activeFrame];
	if (texture->uploadFrame == backend->activeFrame &&
	    texture->uploadSerial == frame->serial) {
		// A single mapped upload slot belongs to this frame. Submit and retire it
		// before using it again so two updates made before Present cannot race.
		if (!finish_commands(backend) || !begin_commands(backend)) {
			return false;
		}
		frame = &backend->frames[backend->activeFrame];
	}

	render_rect_t rect = { 0, 0, static_cast<int>(texture->width),
	                       static_cast<int>(texture->height) };
	if (requested) {
		rect = *requested;
	}
	if (rect.x < 0 || rect.y < 0 || rect.w <= 0 || rect.h <= 0 ||
	    rect.x + rect.w > static_cast<int>(texture->width) ||
	    rect.y + rect.h > static_cast<int>(texture->height)) {
		fprintf(stderr, "DX12: texture update rectangle is outside the texture.\n");
		return false;
	}

	uint8_t *destination = texture->mapped[backend->activeFrame];
	const uint8_t *source = static_cast<const uint8_t *>(pixels);
	for (int y = 0; y < rect.h; ++y) {
		uint8_t *row = destination + static_cast<UINT64>(y) * texture->uploadPitch;
		const uint8_t *sourceRow = source + static_cast<ptrdiff_t>(y) * pitch;
		if (texture->sourceFormat == RENDER_PIXEL_FORMAT_RGBA4444) {
			for (int x = 0; x < rect.w; ++x) {
				const uint16_t packed = static_cast<uint16_t>(sourceRow[x * 2]) |
				                        (static_cast<uint16_t>(sourceRow[x * 2 + 1]) << 8);
				row[x * 4] = static_cast<uint8_t>(((packed >> 4) & 0x0f) * 17);
				row[x * 4 + 1] = static_cast<uint8_t>(((packed >> 8) & 0x0f) * 17);
				row[x * 4 + 2] = static_cast<uint8_t>(((packed >> 12) & 0x0f) * 17);
				row[x * 4 + 3] = static_cast<uint8_t>((packed & 0x0f) * 17);
			}
		} else {
			// BGRA8888 and RGBA8888 each already match their texture's DXGI
			// format byte for byte.
			memcpy(row, sourceRow, static_cast<size_t>(rect.w) * 4);
		}
	}

	transition(backend->commandList, texture->resource, texture->state,
	           D3D12_RESOURCE_STATE_COPY_DEST);
	texture->state = D3D12_RESOURCE_STATE_COPY_DEST;
	D3D12_TEXTURE_COPY_LOCATION sourceLocation = {};
	sourceLocation.pResource = texture->uploads[backend->activeFrame];
	sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	sourceLocation.PlacedFootprint.Footprint.Format = texture->dxgiFormat;
	sourceLocation.PlacedFootprint.Footprint.Width = static_cast<UINT>(rect.w);
	sourceLocation.PlacedFootprint.Footprint.Height = static_cast<UINT>(rect.h);
	sourceLocation.PlacedFootprint.Footprint.Depth = 1;
	sourceLocation.PlacedFootprint.Footprint.RowPitch = texture->uploadPitch;
	D3D12_TEXTURE_COPY_LOCATION destinationLocation = {};
	destinationLocation.pResource = texture->resource;
	destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	backend->commandList->CopyTextureRegion(&destinationLocation, rect.x, rect.y, 0,
	                                         &sourceLocation, NULL);
	transition(backend->commandList, texture->resource, D3D12_RESOURCE_STATE_COPY_DEST,
	           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	texture->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	texture->uploadFrame = backend->activeFrame;
	texture->uploadSerial = frame->serial;
	return true;
}

Vertex *
reserve_vertices(Dx12Backend *backend, UINT count)
{
	if (!begin_commands(backend)) {
		return NULL;
	}
	Dx12Frame *frame = &backend->frames[backend->activeFrame];
	const UINT bytes = count * static_cast<UINT>(sizeof(Vertex));
	if (bytes > kVertexUploadSize - frame->verticesUsed) {
		fprintf(stderr, "DX12: dynamic vertex upload buffer is full.\n");
		return NULL;
	}
	Vertex *vertices = reinterpret_cast<Vertex *>(frame->mappedVertices + frame->verticesUsed);
	frame->verticesUsed += bytes;
	return vertices;
}

void
to_clip_space(Dx12Backend *backend, float x, float y, float *clipX, float *clipY)
{
	const float sx = static_cast<float>(backend->width) / backend->logicalWidth;
	const float sy = static_cast<float>(backend->height) / backend->logicalHeight;
	const float scale = sx < sy ? sx : sy;
	const float contentWidth = backend->logicalWidth * scale;
	const float contentHeight = backend->logicalHeight * scale;
	const float left = (backend->width - contentWidth) * 0.5f;
	const float top = (backend->height - contentHeight) * 0.5f;
	*clipX = ((left + x * scale) / backend->width) * 2.0f - 1.0f;
	*clipY = 1.0f - ((top + y * scale) / backend->height) * 2.0f;
}

void
write_vertex(Dx12Backend *backend, Vertex *vertex, float x, float y, float u, float v,
             render_color_t color, bool applyAlpha)
{
	to_clip_space(backend, x, y, &vertex->x, &vertex->y);
	vertex->u = u;
	vertex->v = v;
	vertex->r = color.r / 255.0f;
	vertex->g = color.g / 255.0f;
	vertex->b = color.b / 255.0f;
	vertex->a = applyAlpha ? color.a / 255.0f : 1.0f;
}

bool
draw_vertices(Dx12Backend *backend, Dx12Texture *texture, Vertex *vertices, UINT count)
{
	if (!vertices || !set_render_target(backend)) {
		return false;
	}
	if (texture->state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
		transition(backend->commandList, texture->resource, texture->state,
		           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		texture->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	}
	ID3D12DescriptorHeap *heaps[] = { backend->srvHeap, backend->samplerHeap };
	backend->commandList->SetDescriptorHeaps(2, heaps);
	backend->commandList->SetGraphicsRootSignature(backend->rootSignature);
	backend->commandList->SetPipelineState(backend->pipeline);
	backend->commandList->SetGraphicsRootDescriptorTable(0,
	                                                      srv_gpu_handle(backend, texture->descriptor));
	D3D12_GPU_DESCRIPTOR_HANDLE sampler = backend->samplerHeap->GetGPUDescriptorHandleForHeapStart();
	const UINT samplerIndex = texture->quality == RENDER_SCALE_NEAREST ? 0 : 1;
	sampler.ptr += static_cast<UINT64>(samplerIndex) * backend->samplerStride;
	backend->commandList->SetGraphicsRootDescriptorTable(1, sampler);
	backend->commandList->SetGraphicsRoot32BitConstant(2, texture->opaque ? 1 : 0, 0);

	Dx12Frame *frame = &backend->frames[backend->activeFrame];
	D3D12_VERTEX_BUFFER_VIEW view = {};
	view.BufferLocation = frame->vertices->GetGPUVirtualAddress() +
	                      reinterpret_cast<uint8_t *>(vertices) - frame->mappedVertices;
	view.SizeInBytes = count * static_cast<UINT>(sizeof(Vertex));
	view.StrideInBytes = sizeof(Vertex);
	backend->commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	backend->commandList->IASetVertexBuffers(0, 1, &view);
	backend->commandList->DrawInstanced(count, 1, 0, 0);
	return true;
}

bool
draw_rectangle(Dx12Backend *backend, Dx12Texture *texture, float x, float y, float width,
               float height, float u0, float v0, float u1, float v1, render_color_t color,
               bool applyAlpha)
{
	Vertex *vertices = reserve_vertices(backend, 6);
	if (!vertices) {
		return false;
	}
	write_vertex(backend, &vertices[0], x, y, u0, v0, color, applyAlpha);
	write_vertex(backend, &vertices[1], x + width, y, u1, v0, color, applyAlpha);
	write_vertex(backend, &vertices[2], x, y + height, u0, v1, color, applyAlpha);
	write_vertex(backend, &vertices[3], x + width, y, u1, v0, color, applyAlpha);
	write_vertex(backend, &vertices[4], x + width, y + height, u1, v1, color, applyAlpha);
	write_vertex(backend, &vertices[5], x, y + height, u0, v1, color, applyAlpha);
	return draw_vertices(backend, texture, vertices, 6);
}

bool
create_render_targets(Dx12Backend *backend)
{
	D3D12_CPU_DESCRIPTOR_HANDLE handle = backend->rtvHeap->GetCPUDescriptorHandleForHeapStart();
	for (UINT i = 0; i < kFrameCount; ++i) {
		HRESULT result = backend->swapChain->GetBuffer(i, IID_PPV_ARGS(&backend->backBuffers[i]));
		if (FAILED(result)) {
			log_hresult("getting swap-chain back buffer", result);
			return false;
		}
		backend->device->CreateRenderTargetView(backend->backBuffers[i], NULL, handle);
		handle.ptr += backend->rtvStride;
	}
	return true;
}

void
release_render_targets(Dx12Backend *backend)
{
	for (UINT i = 0; i < kFrameCount; ++i) {
		release(backend->backBuffers[i]);
	}
}

bool
create_root_signature_and_pipeline(Dx12Backend *backend)
{
	D3D12_DESCRIPTOR_RANGE ranges[2] = {};
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors = 1;
	ranges[0].BaseShaderRegister = 0;
	ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	ranges[1].NumDescriptors = 1;
	ranges[1].BaseShaderRegister = 0;
	ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_ROOT_PARAMETER parameters[3] = {};
	parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	parameters[0].DescriptorTable.NumDescriptorRanges = 1;
	parameters[0].DescriptorTable.pDescriptorRanges = &ranges[0];
	parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	parameters[1].DescriptorTable.NumDescriptorRanges = 1;
	parameters[1].DescriptorTable.pDescriptorRanges = &ranges[1];
	parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	parameters[2].Constants.ShaderRegister = 0;
	parameters[2].Constants.Num32BitValues = 1;

	D3D12_ROOT_SIGNATURE_DESC desc = {};
	desc.NumParameters = 3;
	desc.pParameters = parameters;
	desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	ID3DBlob *serialized = NULL;
	ID3DBlob *errors = NULL;
	HRESULT result = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
	                                              &serialized, &errors);
	if (FAILED(result)) {
		if (errors) {
			fprintf(stderr, "DX12: root signature error: %s\n",
			        static_cast<const char *>(errors->GetBufferPointer()));
		}
		log_hresult("serializing root signature", result);
		release(errors);
		return false;
	}
	result = backend->device->CreateRootSignature(0, serialized->GetBufferPointer(),
	                                               serialized->GetBufferSize(),
	                                               IID_PPV_ARGS(&backend->rootSignature));
	release(serialized);
	release(errors);
	if (FAILED(result)) {
		log_hresult("creating root signature", result);
		return false;
	}

	D3D12_INPUT_ELEMENT_DESC input[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline = {};
	pipeline.pRootSignature = backend->rootSignature;
	pipeline.VS = { g_vs_main, sizeof(g_vs_main) };
	pipeline.PS = { g_ps_main, sizeof(g_ps_main) };
	pipeline.BlendState.RenderTarget[0].BlendEnable = TRUE;
	pipeline.BlendState.RenderTarget[0].LogicOpEnable = FALSE;
	pipeline.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
	pipeline.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	pipeline.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	pipeline.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	pipeline.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	pipeline.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask =
		D3D12_COLOR_WRITE_ENABLE_ALL;
	pipeline.SampleMask = UINT_MAX;
	pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pipeline.RasterizerState.DepthClipEnable = TRUE;
	pipeline.DepthStencilState.DepthEnable = FALSE;
	pipeline.DepthStencilState.StencilEnable = FALSE;
	pipeline.InputLayout = { input, static_cast<UINT>(sizeof(input) / sizeof(input[0])) };
	pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pipeline.NumRenderTargets = 1;
	pipeline.RTVFormats[0] = kFormat;
	pipeline.SampleDesc.Count = 1;
	result = backend->device->CreateGraphicsPipelineState(&pipeline,
	                                                       IID_PPV_ARGS(&backend->pipeline));
	if (FAILED(result)) {
		log_hresult("creating graphics pipeline", result);
		return false;
	}
	return true;
}

bool
create_descriptor_heaps(Dx12Backend *backend)
{
	D3D12_DESCRIPTOR_HEAP_DESC desc = {};
	desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	desc.NumDescriptors = kFrameCount;
	HRESULT result = backend->device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&backend->rtvHeap));
	if (FAILED(result)) {
		log_hresult("creating RTV descriptor heap", result);
		return false;
	}
	backend->rtvStride = backend->device->GetDescriptorHandleIncrementSize(desc.Type);

	desc = {};
	desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	desc.NumDescriptors = kMaxTextures;
	desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	result = backend->device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&backend->srvHeap));
	if (FAILED(result)) {
		log_hresult("creating SRV descriptor heap", result);
		return false;
	}
	backend->srvStride = backend->device->GetDescriptorHandleIncrementSize(desc.Type);

	desc = {};
	desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
	desc.NumDescriptors = 2;
	desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	result = backend->device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&backend->samplerHeap));
	if (FAILED(result)) {
		log_hresult("creating sampler descriptor heap", result);
		return false;
	}
	backend->samplerStride = backend->device->GetDescriptorHandleIncrementSize(desc.Type);
	for (UINT i = 0; i < 2; ++i) {
		D3D12_SAMPLER_DESC sampler = {};
		sampler.Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_POINT :
		                          D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		sampler.MaxLOD = D3D12_FLOAT32_MAX;
		D3D12_CPU_DESCRIPTOR_HANDLE handle =
			backend->samplerHeap->GetCPUDescriptorHandleForHeapStart();
		handle.ptr += static_cast<SIZE_T>(i) * backend->samplerStride;
		backend->device->CreateSampler(&sampler, handle);
	}
	return true;
}

bool
create_vertex_uploads(Dx12Backend *backend)
{
	D3D12_HEAP_PROPERTIES uploadHeap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
	D3D12_RESOURCE_DESC uploadDescription = buffer_desc(kVertexUploadSize);
	for (UINT i = 0; i < kFrameCount; ++i) {
		HRESULT result = backend->device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&backend->frames[i].allocator));
		if (FAILED(result)) {
			log_hresult("creating command allocator", result);
			return false;
		}
		result = backend->device->CreateCommittedResource(
			&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDescription,
			D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
			IID_PPV_ARGS(&backend->frames[i].vertices));
		if (FAILED(result)) {
			log_hresult("creating dynamic vertex upload", result);
			return false;
		}
		D3D12_RANGE readRange = { 0, 0 };
		result = backend->frames[i].vertices->Map(0, &readRange,
		                                           reinterpret_cast<void **>(
			                                           &backend->frames[i].mappedVertices));
		if (FAILED(result)) {
			log_hresult("mapping dynamic vertex upload", result);
			return false;
		}
	}
	return true;
}

bool
create_device(Dx12Backend *backend)
{
	HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&backend->factory));
	if (FAILED(result)) {
		log_hresult("CreateDXGIFactory1", result);
		return false;
	}

	IDXGIFactory6 *factory6 = NULL;
	backend->factory->QueryInterface(IID_PPV_ARGS(&factory6));
	for (UINT index = 0; ; ++index) {
		IDXGIAdapter1 *adapter = NULL;
		if (factory6) {
			result = factory6->EnumAdapterByGpuPreference(
				index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
		} else {
			result = backend->factory->EnumAdapters1(index, &adapter);
		}
		if (result == DXGI_ERROR_NOT_FOUND) {
			break;
		}
		if (FAILED(result)) {
			log_hresult("enumerating adapters", result);
			break;
		}
		DXGI_ADAPTER_DESC1 desc = {};
		adapter->GetDesc1(&desc);
		if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
		    SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
		                                 __uuidof(ID3D12Device), NULL))) {
			result = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
			                           IID_PPV_ARGS(&backend->device));
			adapter->Release();
			if (SUCCEEDED(result)) {
				break;
			}
			log_hresult("creating D3D12 device", result);
		} else {
			adapter->Release();
		}
	}
	release(factory6);
	if (!backend->device) {
		fprintf(stderr, "DX12: no hardware adapter supporting feature level 11_0 was found.\n");
		return false;
	}
	return true;
}

bool
create_swap_chain(Dx12Backend *backend, HWND window)
{
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	HRESULT result = backend->device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&backend->queue));
	if (FAILED(result)) {
		log_hresult("creating direct command queue", result);
		return false;
	}

	BOOL tearing = FALSE;
	IDXGIFactory5 *factory5 = NULL;
	if (SUCCEEDED(backend->factory->QueryInterface(IID_PPV_ARGS(&factory5)))) {
		if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
		                                             &tearing, sizeof(tearing)))) {
			backend->tearing = tearing == TRUE;
		}
		release(factory5);
	}
	DXGI_SWAP_CHAIN_DESC1 desc = {};
	desc.Width = backend->width;
	desc.Height = backend->height;
	desc.Format = kFormat;
	desc.SampleDesc.Count = 1;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = kFrameCount;
	desc.Scaling = DXGI_SCALING_STRETCH;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	desc.Flags = backend->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
	IDXGISwapChain1 *chain = NULL;
	result = backend->factory->CreateSwapChainForHwnd(backend->queue, window, &desc, NULL, NULL,
	                                                   &chain);
	if (FAILED(result)) {
		log_hresult("CreateSwapChainForHwnd", result);
		return false;
	}
	result = chain->QueryInterface(IID_PPV_ARGS(&backend->swapChain));
	release(chain);
	if (FAILED(result)) {
		log_hresult("querying IDXGISwapChain3", result);
		return false;
	}
	result = backend->factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
	if (FAILED(result)) {
		log_hresult("disabling DXGI Alt+Enter", result);
		return false;
	}
	return true;
}

void
update_view(Dx12Backend *backend)
{
	backend->viewport.TopLeftX = 0.0f;
	backend->viewport.TopLeftY = 0.0f;
	backend->viewport.Width = static_cast<float>(backend->width);
	backend->viewport.Height = static_cast<float>(backend->height);
	backend->viewport.MinDepth = 0.0f;
	backend->viewport.MaxDepth = 1.0f;
	backend->scissor.left = 0;
	backend->scissor.top = 0;
	backend->scissor.right = static_cast<LONG>(backend->width);
	backend->scissor.bottom = static_cast<LONG>(backend->height);
}

void
dx12_destroy(render_backend_t *base)
{
	Dx12Backend *backend = reinterpret_cast<Dx12Backend *>(base);
	wait_for_idle_if_available(backend);
	release_texture(backend, backend->whiteTexture);
	backend->whiteTexture = NULL;
	for (UINT i = 0; i < kFrameCount; ++i) {
		if (backend->frames[i].vertices && backend->frames[i].mappedVertices) {
			backend->frames[i].vertices->Unmap(0, NULL);
		}
		release(backend->frames[i].vertices);
		release(backend->frames[i].allocator);
	}
	release_render_targets(backend);
	if (backend->fenceEvent) {
		CloseHandle(backend->fenceEvent);
		backend->fenceEvent = NULL;
	}
	release(backend->fence);
	release(backend->commandList);
	release(backend->pipeline);
	release(backend->rootSignature);
	release(backend->samplerHeap);
	release(backend->srvHeap);
	release(backend->rtvHeap);
	release(backend->swapChain);
	release(backend->queue);
	release(backend->device);
	release(backend->factory);
	delete backend;
}

const char *
dx12_name(render_backend_t *)
{
	return "dx12";
}

bool
dx12_healthy(render_backend_t *base)
{
	return reinterpret_cast<Dx12Backend *>(base)->healthy;
}

bool
dx12_resize(render_backend_t *base, int width, int height)
{
	Dx12Backend *backend = reinterpret_cast<Dx12Backend *>(base);
	if (!backend->healthy) {
		return false;
	}
	if (width <= 0 || height <= 0) {
		return true;
	}
	RECT client = {};
	if (GetClientRect(backend->window, &client)) {
		width = client.right - client.left;
		height = client.bottom - client.top;
	}
	if (width <= 0 || height <= 0) {
		return true;
	}
	if (!finish_commands(backend) || !wait_for_gpu(backend)) {
		return false;
	}
	release_render_targets(backend);
	HRESULT result = backend->swapChain->ResizeBuffers(
		kFrameCount, static_cast<UINT>(width), static_cast<UINT>(height), kFormat,
		backend->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
	if (FAILED(result)) {
		mark_device_lost(backend, "IDXGISwapChain::ResizeBuffers", result);
		return false;
	}
	backend->width = static_cast<UINT>(width);
	backend->height = static_cast<UINT>(height);
	update_view(backend);
	if (!create_render_targets(backend)) {
		backend->healthy = false;
		return false;
	}
	return true;
}

bool
dx12_set_logical_size(render_backend_t *base, int width, int height)
{
	Dx12Backend *backend = reinterpret_cast<Dx12Backend *>(base);
	if (!backend->healthy || width <= 0 || height <= 0) {
		return false;
	}
	backend->logicalWidth = width;
	backend->logicalHeight = height;
	return true;
}

render_texture_t *
dx12_texture_create(render_backend_t *base, render_pixel_format_t format,
                    render_texture_access_t, render_scale_quality_t quality, int width, int height)
{
	Dx12Backend *backend = reinterpret_cast<Dx12Backend *>(base);
	if (!backend->healthy || width <= 0 || height <= 0 ||
	    (format != RENDER_PIXEL_FORMAT_BGRA8888 && format != RENDER_PIXEL_FORMAT_RGBA4444 &&
	     format != RENDER_PIXEL_FORMAT_RGBA8888)) {
		return NULL;
	}
	Dx12Texture *texture = new (std::nothrow) Dx12Texture();
	if (!texture) {
		fprintf(stderr, "DX12: out of memory creating texture.\n");
		return NULL;
	}
	texture->base.backend = base;
	texture->width = static_cast<UINT>(width);
	texture->height = static_cast<UINT>(height);
	texture->sourceFormat = format;
	texture->dxgiFormat = dxgi_format_for(format);
	texture->quality = quality;
	texture->color = { 255, 255, 255, 255 };
	texture->state = D3D12_RESOURCE_STATE_COPY_DEST;
	texture->opaque = format == RENDER_PIXEL_FORMAT_BGRA8888;
	if (!allocate_descriptor(backend, &texture->descriptor)) {
		release_texture(backend, texture);
		return NULL;
	}
	texture->hasDescriptor = true;
	if (!create_texture_resources(backend, texture)) {
		release_texture(backend, texture);
		return NULL;
	}
	return &texture->base;
}

void
dx12_texture_destroy(render_texture_t *base)
{
	Dx12Texture *texture = reinterpret_cast<Dx12Texture *>(base);
	Dx12Backend *backend = reinterpret_cast<Dx12Backend *>(texture->base.backend);
	wait_for_idle_if_available(backend);
	release_texture(backend, texture);
}

bool
dx12_texture_update(render_texture_t *base, const render_rect_t *rect,
                    const void *pixels, int pitch)
{
	return update_texture(reinterpret_cast<Dx12Texture *>(base), rect, pixels, pitch);
}

bool
dx12_texture_set_color(render_texture_t *base, render_color_t color)
{
	Dx12Texture *texture = reinterpret_cast<Dx12Texture *>(base);
	Dx12Backend *backend = reinterpret_cast<Dx12Backend *>(texture->base.backend);
	if (!backend->healthy) {
		return false;
	}
	texture->color = color;
	return true;
}

bool
dx12_set_draw_color(render_backend_t *base, render_color_t color)
{
	Dx12Backend *backend = reinterpret_cast<Dx12Backend *>(base);
	if (!backend->healthy) {
		return false;
	}
	backend->whiteTexture->color = color;
	return true;
}

bool
dx12_clear(render_backend_t *base)
{
	Dx12Backend *backend = reinterpret_cast<Dx12Backend *>(base);
	if (!set_render_target(backend)) {
		return false;
	}
	const render_color_t color = backend->whiteTexture->color;
	const float clear[] = { color.r / 255.0f, color.g / 255.0f, color.b / 255.0f,
	                        color.a / 255.0f };
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = backend->rtvHeap->GetCPUDescriptorHandleForHeapStart();
	rtv.ptr += static_cast<SIZE_T>(backend->activeFrame) * backend->rtvStride;
	backend->commandList->ClearRenderTargetView(rtv, clear, 0, NULL);
	return true;
}

bool
dx12_copy(render_backend_t *base, render_texture_t *sourceBase,
          const render_rect_t *sourceRect, const render_rect_t *destinationRect)
{
	Dx12Backend *backend = reinterpret_cast<Dx12Backend *>(base);
	Dx12Texture *source = reinterpret_cast<Dx12Texture *>(sourceBase);
	render_rect_t input = { 0, 0, static_cast<int>(source->width), static_cast<int>(source->height) };
	if (sourceRect) {
		input = *sourceRect;
	}
	render_rect_t output = { 0, 0, backend->logicalWidth, backend->logicalHeight };
	if (destinationRect) {
		output = *destinationRect;
	}
	if (input.x < 0 || input.y < 0 || input.w <= 0 || input.h <= 0 ||
	    input.x + input.w > static_cast<int>(source->width) ||
	    input.y + input.h > static_cast<int>(source->height) ||
	    output.w <= 0 || output.h <= 0) {
		return false;
	}
	const float u0 = input.x / static_cast<float>(source->width);
	const float v0 = input.y / static_cast<float>(source->height);
	const float u1 = (input.x + input.w) / static_cast<float>(source->width);
	const float v1 = (input.y + input.h) / static_cast<float>(source->height);
	return draw_rectangle(backend, source, static_cast<float>(output.x), static_cast<float>(output.y),
	                      static_cast<float>(output.w), static_cast<float>(output.h),
	                      u0, v0, u1, v1, source->color, false);
}

bool
dx12_fill_rect(render_backend_t *base, const render_rect_t *requested)
{
	Dx12Backend *backend = reinterpret_cast<Dx12Backend *>(base);
	render_rect_t rect = { 0, 0, backend->logicalWidth, backend->logicalHeight };
	if (requested) {
		rect = *requested;
	}
	if (rect.w <= 0 || rect.h <= 0) {
		return false;
	}
	return draw_rectangle(backend, backend->whiteTexture, static_cast<float>(rect.x),
	                      static_cast<float>(rect.y), static_cast<float>(rect.w),
	                      static_cast<float>(rect.h), 0.0f, 0.0f, 1.0f, 1.0f,
	                      backend->whiteTexture->color, true);
}

bool
dx12_draw_line(render_backend_t *base, int x1, int y1, int x2, int y2)
{
	Dx12Backend *backend = reinterpret_cast<Dx12Backend *>(base);
	const float dx = static_cast<float>(x2 - x1);
	const float dy = static_cast<float>(y2 - y1);
	const float length = sqrtf(dx * dx + dy * dy);
	if (length == 0.0f) {
		render_rect_t point = { x1, y1, 1, 1 };
		return dx12_fill_rect(base, &point);
	}
	const float offsetX = -dy * 0.5f / length;
	const float offsetY = dx * 0.5f / length;
	Vertex *vertices = reserve_vertices(backend, 6);
	if (!vertices) {
		return false;
	}
	const render_color_t color = backend->whiteTexture->color;
	write_vertex(backend, &vertices[0], x1 + offsetX, y1 + offsetY, 0, 0, color, true);
	write_vertex(backend, &vertices[1], x2 + offsetX, y2 + offsetY, 1, 0, color, true);
	write_vertex(backend, &vertices[2], x1 - offsetX, y1 - offsetY, 0, 1, color, true);
	write_vertex(backend, &vertices[3], x2 + offsetX, y2 + offsetY, 1, 0, color, true);
	write_vertex(backend, &vertices[4], x2 - offsetX, y2 - offsetY, 1, 1, color, true);
	write_vertex(backend, &vertices[5], x1 - offsetX, y1 - offsetY, 0, 1, color, true);
	return draw_vertices(backend, backend->whiteTexture, vertices, 6);
}

void
dx12_present(render_backend_t *base)
{
	Dx12Backend *backend = reinterpret_cast<Dx12Backend *>(base);
	if (!backend->healthy || !backend->commandListOpen) {
		return;
	}
	if (backend->backBufferIsRenderTarget) {
		transition(backend->commandList, backend->backBuffers[backend->activeFrame],
		           D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
		backend->backBufferIsRenderTarget = false;
	}
	HRESULT result = backend->commandList->Close();
	if (FAILED(result)) {
		mark_device_lost(backend, "ID3D12GraphicsCommandList::Close", result);
		return;
	}
	ID3D12CommandList *lists[] = { backend->commandList };
	backend->queue->ExecuteCommandLists(1, lists);
	backend->commandListOpen = false;
	result = backend->swapChain->Present(0, backend->tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
	if (FAILED(result)) {
		mark_device_lost(backend, "IDXGISwapChain::Present", result);
		return;
	}
	const UINT64 fenceValue = backend->nextFenceValue++;
	result = backend->queue->Signal(backend->fence, fenceValue);
	if (FAILED(result)) {
		mark_device_lost(backend, "ID3D12CommandQueue::Signal", result);
		return;
	}
	backend->frames[backend->activeFrame].fenceValue = fenceValue;
}

const render_backend_ops_t dx12_ops = {
	dx12_destroy,
	dx12_name,
	dx12_healthy,
	dx12_resize,
	dx12_set_logical_size,
	dx12_texture_create,
	dx12_texture_destroy,
	dx12_texture_update,
	dx12_texture_set_color,
	dx12_set_draw_color,
	dx12_clear,
	dx12_copy,
	dx12_fill_rect,
	dx12_draw_line,
	dx12_present
};

} // namespace

// ── Shared-device interop (render_dx12_interop.h) ───────────────────────────
// The ImGui debugger renders into a second swap chain on this backend's device
// and queue. Everything it needs is handed out here, behind an ops-table check
// so an SDL backend can never be mistaken for a native one.
namespace {

Dx12Backend *
native_backend(render_backend_t *base)
{
	if (!base || base->ops != &dx12_ops) {
		return NULL;
	}
	Dx12Backend *backend = reinterpret_cast<Dx12Backend *>(base);
	return backend->healthy ? backend : NULL;
}

} // namespace

bool
render_dx12_is_native(render_backend_t *base)
{
	return native_backend(base) != NULL;
}

ID3D12Device *
render_dx12_device(render_backend_t *base)
{
	Dx12Backend *backend = native_backend(base);
	return backend ? backend->device : NULL;
}

ID3D12CommandQueue *
render_dx12_queue(render_backend_t *base)
{
	Dx12Backend *backend = native_backend(base);
	return backend ? backend->queue : NULL;
}

ID3D12DescriptorHeap *
render_dx12_srv_heap(render_backend_t *base)
{
	Dx12Backend *backend = native_backend(base);
	return backend ? backend->srvHeap : NULL;
}

bool
render_dx12_srv_allocate(render_backend_t *base, unsigned int *slot)
{
	Dx12Backend *backend = native_backend(base);
	if (!backend || !slot) {
		return false;
	}
	UINT descriptor = 0;
	if (!allocate_descriptor(backend, &descriptor)) {
		return false;
	}
	*slot = descriptor;
	return true;
}

void
render_dx12_srv_free(render_backend_t *base, unsigned int slot)
{
	// Deliberately not gated on healthy: a slot still has to go back to the
	// free list when the device was lost, or a later recovery leaks it.
	if (!base || base->ops != &dx12_ops) {
		return;
	}
	free_descriptor(reinterpret_cast<Dx12Backend *>(base), static_cast<UINT>(slot));
}

D3D12_CPU_DESCRIPTOR_HANDLE
render_dx12_srv_cpu_handle(render_backend_t *base, unsigned int slot)
{
	Dx12Backend *backend = native_backend(base);
	if (!backend) {
		D3D12_CPU_DESCRIPTOR_HANDLE empty = {};
		return empty;
	}
	return srv_cpu_handle(backend, static_cast<UINT>(slot));
}

D3D12_GPU_DESCRIPTOR_HANDLE
render_dx12_srv_gpu_handle(render_backend_t *base, unsigned int slot)
{
	Dx12Backend *backend = native_backend(base);
	if (!backend) {
		D3D12_GPU_DESCRIPTOR_HANDLE empty = {};
		return empty;
	}
	return srv_gpu_handle(backend, static_cast<UINT>(slot));
}

bool
render_dx12_texture_gpu_handle(render_texture_t *base, D3D12_GPU_DESCRIPTOR_HANDLE *handle)
{
	if (!base || !handle) {
		return false;
	}
	Dx12Backend *backend = native_backend(base->backend);
	if (!backend) {
		return false;
	}
	Dx12Texture *texture = reinterpret_cast<Dx12Texture *>(base);
	if (!texture->hasDescriptor) {
		return false;
	}
	*handle = srv_gpu_handle(backend, texture->descriptor);
	return true;
}

bool
render_dx12_flush_pending(render_backend_t *base)
{
	Dx12Backend *backend = native_backend(base);
	if (!backend) {
		return false;
	}
	return finish_commands(backend);
}

bool
render_dx12_wait_idle(render_backend_t *base)
{
	if (!base || base->ops != &dx12_ops) {
		return false;
	}
	Dx12Backend *backend = reinterpret_cast<Dx12Backend *>(base);
	wait_for_idle_if_available(backend);
	return backend->healthy;
}

extern "C" render_backend_t *
render_backend_create_dx12(SDL_Window *window)
{
	if (!window) {
		return NULL;
	}
	SDL_SysWMinfo info = {};
	SDL_VERSION(&info.version);
	if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WINDOWS ||
	    !info.info.win.window) {
		fprintf(stderr, "DX12: could not get the SDL window HWND: %s\n", SDL_GetError());
		return NULL;
	}
	RECT client = {};
	if (!GetClientRect(info.info.win.window, &client) || client.right <= client.left ||
	    client.bottom <= client.top) {
		fprintf(stderr, "DX12: SDL window has no usable client area.\n");
		return NULL;
	}

	Dx12Backend *backend = new (std::nothrow) Dx12Backend();
	if (!backend) {
		fprintf(stderr, "DX12: out of memory creating renderer.\n");
		return NULL;
	}
	backend->base.ops = &dx12_ops;
	backend->window = info.info.win.window;
	backend->width = static_cast<UINT>(client.right - client.left);
	backend->height = static_cast<UINT>(client.bottom - client.top);
	backend->logicalWidth = static_cast<int>(backend->width);
	backend->logicalHeight = static_cast<int>(backend->height);
	backend->nextFenceValue = 1;
	backend->healthy = true;
	update_view(backend);

	if (!create_device(backend) || !create_swap_chain(backend, info.info.win.window) ||
	    !create_descriptor_heaps(backend) || !create_render_targets(backend) ||
	    !create_vertex_uploads(backend) || !create_root_signature_and_pipeline(backend)) {
		dx12_destroy(&backend->base);
		return NULL;
	}
	HRESULT result = backend->device->CreateCommandList(
		0, D3D12_COMMAND_LIST_TYPE_DIRECT, backend->frames[0].allocator, NULL,
		IID_PPV_ARGS(&backend->commandList));
	if (FAILED(result)) {
		log_hresult("creating graphics command list", result);
		dx12_destroy(&backend->base);
		return NULL;
	}
	result = backend->commandList->Close();
	if (FAILED(result)) {
		log_hresult("closing initial graphics command list", result);
		dx12_destroy(&backend->base);
		return NULL;
	}
	result = backend->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&backend->fence));
	if (FAILED(result)) {
		log_hresult("creating fence", result);
		dx12_destroy(&backend->base);
		return NULL;
	}
	backend->fenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	if (!backend->fenceEvent) {
		fprintf(stderr, "DX12: creating fence event failed.\n");
		dx12_destroy(&backend->base);
		return NULL;
	}

	render_texture_t *white = dx12_texture_create(&backend->base, RENDER_PIXEL_FORMAT_BGRA8888,
	                                               RENDER_TEXTURE_STATIC,
	                                               RENDER_SCALE_NEAREST, 1, 1);
	if (!white) {
		dx12_destroy(&backend->base);
		return NULL;
	}
	backend->whiteTexture = reinterpret_cast<Dx12Texture *>(white);
	const uint8_t whitePixel[] = { 255, 255, 255, 0 };
	if (!dx12_texture_update(white, NULL, whitePixel, 4)) {
		dx12_destroy(&backend->base);
		return NULL;
	}
	// SDL renderers start with a transparent-black draw color.
	backend->whiteTexture->color = { 0, 0, 0, 0 };
	return &backend->base;
}
