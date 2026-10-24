// Commander X16 Emulator — native DirectX 12 renderer for the ImGui debugger.
//
// See debug_ui_dx12.h. This file owns only what is per-window: the debugger's
// swap chain, its RTV heap, its command allocators/list and its fence. The
// device, the direct command queue and the shader-visible SRV heap are borrowed
// from the emulator's main-window backend through render_dx12_interop.h, so
// panel textures created on that backend can be handed straight to ImGui as
// descriptor handles and there is only ever one D3D12 device in the process.
//
// Sharing the queue is also what keeps texture uploads correct: the main
// backend records them into its own command list, so this renderer submits that
// list (render_dx12_flush_pending) before its own, and same-queue ordering makes
// the uploads visible to the draw that samples them.
#include "debug_ui_dx12.h"

#ifdef HAS_DX12

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"

#include "render_dx12_interop.h"

#include <SDL.h>
#include <SDL_syswm.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <stdio.h>
#include <new>

namespace {

constexpr UINT kFrameCount = 3;
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

struct DebugUiDx12 {
    HWND window = NULL;
    render_backend_t *shared = NULL;
    // Borrowed from the main backend; never released here.
    ID3D12Device *device = NULL;
    ID3D12CommandQueue *queue = NULL;
    ID3D12DescriptorHeap *srvHeap = NULL;
    // Owned.
    IDXGIFactory4 *factory = NULL;
    IDXGISwapChain3 *swapChain = NULL;
    ID3D12DescriptorHeap *rtvHeap = NULL;
    ID3D12Resource *backBuffers[kFrameCount] = {};
    ID3D12CommandAllocator *allocators[kFrameCount] = {};
    ID3D12GraphicsCommandList *commandList = NULL;
    ID3D12Fence *fence = NULL;
    HANDLE fenceEvent = NULL;
    UINT64 fenceValues[kFrameCount] = {};
    UINT64 nextFenceValue = 1;
    UINT rtvStride = 0;
    UINT width = 0;
    UINT height = 0;
    UINT fontSlot = 0;
    bool hasFontSlot = false;
    bool tearing = false;
    bool rendererBackendReady = false;
    bool healthy = false;
};

DebugUiDx12 *s_dx12 = NULL;

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
    fprintf(stderr, "DX12 debugger: %s failed (0x%08lX).\n", operation,
            static_cast<unsigned long>(result));
}

bool
client_size(HWND window, UINT *width, UINT *height)
{
    RECT client = {};
    if (!GetClientRect(window, &client)) {
        return false;
    }
    const LONG w = client.right - client.left;
    const LONG h = client.bottom - client.top;
    if (w <= 0 || h <= 0) {
        // Minimized, or a zero-size window: nothing to present into.
        return false;
    }
    *width = static_cast<UINT>(w);
    *height = static_cast<UINT>(h);
    return true;
}

bool
wait_for_fence(DebugUiDx12 *ui, UINT64 value)
{
    if (!value || ui->fence->GetCompletedValue() >= value) {
        return true;
    }
    HRESULT result = ui->fence->SetEventOnCompletion(value, ui->fenceEvent);
    if (FAILED(result)) {
        log_hresult("ID3D12Fence::SetEventOnCompletion", result);
        ui->healthy = false;
        return false;
    }
    if (WaitForSingleObject(ui->fenceEvent, INFINITE) != WAIT_OBJECT_0) {
        fprintf(stderr, "DX12 debugger: waiting for the GPU fence failed.\n");
        ui->healthy = false;
        return false;
    }
    return true;
}

// Signal on the shared queue and wait. Because the main backend submits to the
// same queue, this also retires its work, which is what makes it safe to
// recycle anything shared.
bool
wait_for_gpu(DebugUiDx12 *ui)
{
    if (!ui->queue || !ui->fence) {
        return true;
    }
    const UINT64 value = ui->nextFenceValue++;
    HRESULT result = ui->queue->Signal(ui->fence, value);
    if (FAILED(result)) {
        log_hresult("ID3D12CommandQueue::Signal", result);
        ui->healthy = false;
        return false;
    }
    return wait_for_fence(ui, value);
}

void
release_render_targets(DebugUiDx12 *ui)
{
    for (UINT i = 0; i < kFrameCount; ++i) {
        release(ui->backBuffers[i]);
    }
}

bool
create_render_targets(DebugUiDx12 *ui)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = ui->rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i) {
        HRESULT result = ui->swapChain->GetBuffer(i, IID_PPV_ARGS(&ui->backBuffers[i]));
        if (FAILED(result)) {
            log_hresult("getting swap-chain back buffer", result);
            return false;
        }
        ui->device->CreateRenderTargetView(ui->backBuffers[i], NULL, handle);
        handle.ptr += ui->rtvStride;
    }
    return true;
}

bool
resize_swap_chain(DebugUiDx12 *ui, UINT width, UINT height)
{
    if (!wait_for_gpu(ui)) {
        return false;
    }
    release_render_targets(ui);
    const UINT flags = ui->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    HRESULT result = ui->swapChain->ResizeBuffers(kFrameCount, width, height, kFormat, flags);
    if (FAILED(result)) {
        log_hresult("IDXGISwapChain::ResizeBuffers", result);
        ui->healthy = false;
        return false;
    }
    ui->width = width;
    ui->height = height;
    if (!create_render_targets(ui)) {
        ui->healthy = false;
        return false;
    }
    // Every frame slot was retired by the wait above.
    for (UINT i = 0; i < kFrameCount; ++i) {
        ui->fenceValues[i] = 0;
    }
    return true;
}

bool
create_swap_chain(DebugUiDx12 *ui)
{
    HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&ui->factory));
    if (FAILED(result)) {
        log_hresult("CreateDXGIFactory1", result);
        return false;
    }

    BOOL tearing = FALSE;
    IDXGIFactory5 *factory5 = NULL;
    if (SUCCEEDED(ui->factory->QueryInterface(IID_PPV_ARGS(&factory5)))) {
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                    &tearing, sizeof(tearing)))) {
            ui->tearing = tearing == TRUE;
        }
        release(factory5);
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = ui->width;
    desc.Height = ui->height;
    desc.Format = kFormat;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kFrameCount;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = ui->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    IDXGISwapChain1 *chain = NULL;
    result = ui->factory->CreateSwapChainForHwnd(ui->queue, ui->window, &desc, NULL, NULL,
                                                 &chain);
    if (FAILED(result)) {
        log_hresult("CreateSwapChainForHwnd", result);
        return false;
    }
    result = chain->QueryInterface(IID_PPV_ARGS(&ui->swapChain));
    release(chain);
    if (FAILED(result)) {
        log_hresult("querying IDXGISwapChain3", result);
        return false;
    }
    ui->factory->MakeWindowAssociation(ui->window, DXGI_MWA_NO_ALT_ENTER);
    return true;
}

bool
create_frame_resources(DebugUiDx12 *ui)
{
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = kFrameCount;
    HRESULT result = ui->device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&ui->rtvHeap));
    if (FAILED(result)) {
        log_hresult("creating RTV descriptor heap", result);
        return false;
    }
    ui->rtvStride = ui->device->GetDescriptorHandleIncrementSize(rtvDesc.Type);

    for (UINT i = 0; i < kFrameCount; ++i) {
        result = ui->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&ui->allocators[i]));
        if (FAILED(result)) {
            log_hresult("creating command allocator", result);
            return false;
        }
    }
    result = ui->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           ui->allocators[0], NULL,
                                           IID_PPV_ARGS(&ui->commandList));
    if (FAILED(result)) {
        log_hresult("creating graphics command list", result);
        return false;
    }
    result = ui->commandList->Close();
    if (FAILED(result)) {
        log_hresult("closing initial graphics command list", result);
        return false;
    }
    result = ui->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ui->fence));
    if (FAILED(result)) {
        log_hresult("creating fence", result);
        return false;
    }
    ui->fenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!ui->fenceEvent) {
        fprintf(stderr, "DX12 debugger: creating fence event failed.\n");
        return false;
    }
    return create_render_targets(ui);
}

void
transition(ID3D12GraphicsCommandList *commandList, ID3D12Resource *resource,
           D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);
}

// Release everything this renderer owns. `waitForGpu` is skipped when the
// caller already knows the device is gone.
void
destroy(DebugUiDx12 *ui)
{
    if (!ui) {
        return;
    }
    if (ui->device && SUCCEEDED(ui->device->GetDeviceRemovedReason())) {
        wait_for_gpu(ui);
    }
    if (ui->rendererBackendReady) {
        ImGui_ImplDX12_Shutdown();
        ui->rendererBackendReady = false;
    }
    if (ui->hasFontSlot) {
        render_dx12_srv_free(ui->shared, ui->fontSlot);
        ui->hasFontSlot = false;
    }
    release_render_targets(ui);
    if (ui->fenceEvent) {
        CloseHandle(ui->fenceEvent);
        ui->fenceEvent = NULL;
    }
    release(ui->fence);
    release(ui->commandList);
    for (UINT i = 0; i < kFrameCount; ++i) {
        release(ui->allocators[i]);
    }
    release(ui->rtvHeap);
    release(ui->swapChain);
    release(ui->factory);
    delete ui;
}

} // namespace

extern "C" bool
debug_ui_dx12_init(SDL_Window *window, render_backend_t *backend)
{
    if (s_dx12) {
        return true;
    }
    if (!window || !render_dx12_is_native(backend)) {
        return false;
    }

    SDL_SysWMinfo info = {};
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WINDOWS ||
        !info.info.win.window) {
        fprintf(stderr, "DX12 debugger: could not get the SDL window HWND: %s\n",
                SDL_GetError());
        return false;
    }

    DebugUiDx12 *ui = new (std::nothrow) DebugUiDx12();
    if (!ui) {
        fprintf(stderr, "DX12 debugger: out of memory creating renderer.\n");
        return false;
    }
    ui->window = info.info.win.window;
    ui->shared = backend;
    ui->device = render_dx12_device(backend);
    ui->queue = render_dx12_queue(backend);
    ui->srvHeap = render_dx12_srv_heap(backend);
    if (!ui->device || !ui->queue || !ui->srvHeap) {
        fprintf(stderr, "DX12 debugger: the main backend has no shareable device.\n");
        destroy(ui);
        return false;
    }
    if (!client_size(ui->window, &ui->width, &ui->height)) {
        // The window has just been created and shown, so a zero client area
        // here means something is wrong rather than merely minimized.
        fprintf(stderr, "DX12 debugger: the debugger window has no usable client area.\n");
        destroy(ui);
        return false;
    }
    ui->healthy = true;

    if (!create_swap_chain(ui) || !create_frame_resources(ui)) {
        destroy(ui);
        return false;
    }

    if (!render_dx12_srv_allocate(backend, &ui->fontSlot)) {
        fprintf(stderr, "DX12 debugger: no free slot in the shared SRV heap.\n");
        destroy(ui);
        return false;
    }
    ui->hasFontSlot = true;

    if (!ImGui_ImplDX12_Init(ui->device, static_cast<int>(kFrameCount), kFormat, ui->srvHeap,
                             render_dx12_srv_cpu_handle(backend, ui->fontSlot),
                             render_dx12_srv_gpu_handle(backend, ui->fontSlot))) {
        fprintf(stderr, "DX12 debugger: ImGui_ImplDX12_Init failed.\n");
        destroy(ui);
        return false;
    }
    ui->rendererBackendReady = true;

    // Build the pipeline and the font atlas up front rather than letting
    // ImGui_ImplDX12_NewFrame do it lazily, so a failure is reported here and
    // the caller can still fall back to the SDLRenderer2 path.
    if (!ImGui_ImplDX12_CreateDeviceObjects()) {
        fprintf(stderr, "DX12 debugger: creating the ImGui device objects failed.\n");
        destroy(ui);
        return false;
    }

    s_dx12 = ui;
    return true;
}

extern "C" void
debug_ui_dx12_shutdown(void)
{
    destroy(s_dx12);
    s_dx12 = NULL;
}

extern "C" bool
debug_ui_dx12_active(void)
{
    return s_dx12 != NULL;
}

extern "C" render_backend_t *
debug_ui_dx12_backend(void)
{
    return s_dx12 ? s_dx12->shared : NULL;
}

extern "C" bool
debug_ui_dx12_render(void)
{
    DebugUiDx12 *ui = s_dx12;
    if (!ui) {
        return false;
    }
    if (!ui->healthy) {
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    if (!client_size(ui->window, &width, &height)) {
        // Minimized or zero-sized: skip the frame, but the debugger is fine.
        return true;
    }
    if ((width != ui->width || height != ui->height) && !resize_swap_chain(ui, width, height)) {
        return false;
    }

    // Submit whatever the main backend has recorded (panel texture uploads
    // among it) before this frame's command list, so the same-queue ordering
    // makes those uploads visible to the draws below.
    if (!render_dx12_flush_pending(ui->shared)) {
        fprintf(stderr, "DX12 debugger: the shared backend could not flush its commands.\n");
        return false;
    }

    const UINT index = ui->swapChain->GetCurrentBackBufferIndex();
    if (!wait_for_fence(ui, ui->fenceValues[index])) {
        return false;
    }
    HRESULT result = ui->allocators[index]->Reset();
    if (FAILED(result)) {
        log_hresult("ID3D12CommandAllocator::Reset", result);
        ui->healthy = false;
        return false;
    }
    result = ui->commandList->Reset(ui->allocators[index], NULL);
    if (FAILED(result)) {
        log_hresult("ID3D12GraphicsCommandList::Reset", result);
        ui->healthy = false;
        return false;
    }

    transition(ui->commandList, ui->backBuffers[index], D3D12_RESOURCE_STATE_PRESENT,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ui->rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(index) * ui->rtvStride;
    // The same neutral grey the SDLRenderer2 path clears to.
    const float clear[] = { 24.0f / 255.0f, 24.0f / 255.0f, 24.0f / 255.0f, 1.0f };
    ui->commandList->OMSetRenderTargets(1, &rtv, FALSE, NULL);
    ui->commandList->ClearRenderTargetView(rtv, clear, 0, NULL);
    // ImGui's root signature uses a static sampler, so the SRV heap is the only
    // one it needs -- and it is the shared one, which is where both the font
    // atlas and every panel texture live.
    ui->commandList->SetDescriptorHeaps(1, &ui->srvHeap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), ui->commandList);
    transition(ui->commandList, ui->backBuffers[index], D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_PRESENT);

    result = ui->commandList->Close();
    if (FAILED(result)) {
        log_hresult("ID3D12GraphicsCommandList::Close", result);
        ui->healthy = false;
        return false;
    }
    ID3D12CommandList *lists[] = { ui->commandList };
    ui->queue->ExecuteCommandLists(1, lists);
    // No vsync, for the same reason the SDL debugger renderer does not use
    // SDL_RENDERER_PRESENTVSYNC: this is presented from the emulator thread.
    result = ui->swapChain->Present(0, ui->tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (FAILED(result)) {
        log_hresult("IDXGISwapChain::Present", result);
        ui->healthy = false;
        return false;
    }
    const UINT64 value = ui->nextFenceValue++;
    result = ui->queue->Signal(ui->fence, value);
    if (FAILED(result)) {
        log_hresult("ID3D12CommandQueue::Signal", result);
        ui->healthy = false;
        return false;
    }
    ui->fenceValues[index] = value;
    return true;
}

#endif // HAS_DX12
