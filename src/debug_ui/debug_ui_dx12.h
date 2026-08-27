// Commander X16 Emulator — native DirectX 12 renderer for the ImGui debugger.
//
// A compact second swap chain for the debugger window, sharing the main
// window's D3D12 device, direct command queue and shader-visible SRV heap (see
// render_dx12_interop.h). Only the per-window resources -- swap chain, RTV
// heap, command allocators, command list and fence -- are owned here.
//
// Used only when the emulator output itself is running on the native DX12
// backend (`-render dx12`). Every entry point is a safe no-op otherwise, and
// debug_ui.cpp falls back to the SDLRenderer2 path whenever init or a frame
// fails.
#ifndef DEBUG_UI_DX12_H
#define DEBUG_UI_DX12_H

#include <stdbool.h>

#include "render_backend.h"

typedef struct SDL_Window SDL_Window;

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HAS_DX12

// Bring up the debugger swap chain on `backend`'s device/queue and initialize
// the ImGui DX12 renderer backend against the shared SRV heap. `backend` must
// be the native DX12 main-window backend; anything else is rejected. The ImGui
// context and the SDL2 platform backend must already exist. Cleans up after
// itself on failure, leaving nothing half-initialized.
bool debug_ui_dx12_init(SDL_Window *window, render_backend_t *backend);

// Shut down the ImGui DX12 renderer backend and release the debugger's own
// objects, after waiting for the shared queue. Idempotent, and safe to call
// with a lost device. Must run before the shared backend is destroyed.
void debug_ui_dx12_shutdown(void);

// True between a successful debug_ui_dx12_init and debug_ui_dx12_shutdown.
bool debug_ui_dx12_active(void);

// Present one debugger frame: flush the main backend's pending uploads, clear,
// draw the current ImGui draw data and Present. Call after ImGui::Render().
// False means the debugger swap chain has failed and the caller should fall
// back to the SDLRenderer2 path.
bool debug_ui_dx12_render(void);

// The main-window backend the debugger borrowed its device from, or NULL.
render_backend_t *debug_ui_dx12_backend(void);

#endif // HAS_DX12

#ifdef __cplusplus
} // extern "C"
#endif

#endif // DEBUG_UI_DX12_H
