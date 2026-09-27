// Commander X16 Emulator — debugger panel image textures.
//
// Panels that show decoded pixels (the VERA viewers) need a GPU texture they
// can hand to ImGui::Image as an ImTextureID. Which kind of texture that is
// depends on how the debugger window is being drawn:
//
//   * SDLRenderer2 path — a STREAMING SDL_Texture on the debugger's
//     SDL_Renderer, exactly as before.
//   * native DX12 path — an RGBA8888 render_texture_t on the emulator's shared
//     D3D12 backend, whose SRV lives in the shared shader-visible heap, so the
//     ImTextureID is simply its GPU descriptor handle.
//
// Callers only ever see ensure()/update()/id(), so the ImGui::Image call sites
// are identical either way.
//
// Lifetime: every live DebugUiImage is on an intrusive list, so the debugger
// can release all GPU objects at a well-defined moment --
// debug_ui_image_release_all() -- before the renderer or the shared backend
// goes away. Panel images are function-statics that outlive both, and this is
// what stops them retaining a stale backend pointer across a fallback.
#ifndef DEBUG_UI_IMAGE_H
#define DEBUG_UI_IMAGE_H

#include <stdint.h>

#include "imgui.h"

struct SDL_Renderer;
struct SDL_Texture;
struct render_backend;
struct render_texture;

// Select the backing store. Called by debug_ui.cpp as it brings a renderer up;
// each implicitly releases whatever the previous mode had allocated.
void debug_ui_image_use_sdl(SDL_Renderer *renderer);
void debug_ui_image_use_dx12(struct render_backend *backend);

// Destroy every live image's GPU object and forget the current mode. Must run
// before the SDL renderer is destroyed or the shared DX12 backend is torn down.
void debug_ui_image_release_all(void);

// False when no backing store is available, i.e. panels should say so rather
// than draw.
bool debug_ui_image_available(void);

class DebugUiImage {
public:
    DebugUiImage();
    ~DebugUiImage();

    DebugUiImage(const DebugUiImage &) = delete;
    DebugUiImage &operator=(const DebugUiImage &) = delete;

    // Create or resize to width x height. False if that is not possible, in
    // which case the caller must not draw.
    bool ensure(int width, int height);

    // Upload a full width*height RGBA8888 image (0xAABBGGRR words).
    void update(const uint32_t *pixels);

    // The ImTextureID for ImGui::Image. Zero until ensure() has succeeded.
    ImTextureID id() const;

private:
    friend void debug_ui_image_release_all(void);

    void release();

    SDL_Texture           *m_sdl     = nullptr;
    struct render_texture *m_native  = nullptr;
    ImTextureID            m_id      = 0;
    int                    m_width   = 0;
    int                    m_height  = 0;
    DebugUiImage          *m_next    = nullptr;
};

#endif // DEBUG_UI_IMAGE_H
