// Commander X16 Emulator — debugger panel image textures. See debug_ui_image.h.
#include "debug_ui_image.h"

#include "render_backend.h"

#ifdef HAS_DX12
#include "render_dx12_interop.h"
#endif

#include <SDL.h>

namespace {

enum class ImageMode {
    None,
    Sdl,
    Dx12
};

// Plain globals with no destructors of their own: panel images are
// function-statics whose destructors run during static teardown, by which point
// anything non-trivial here could already have been destroyed.
ImageMode         s_mode     = ImageMode::None;
SDL_Renderer     *s_renderer = nullptr;
render_backend_t *s_backend  = nullptr;
DebugUiImage     *s_images   = nullptr;

} // namespace

void
debug_ui_image_release_all(void)
{
    for (DebugUiImage *image = s_images; image; image = image->m_next) {
        image->release();
    }
    s_mode     = ImageMode::None;
    s_renderer = nullptr;
    s_backend  = nullptr;
}

void
debug_ui_image_use_sdl(SDL_Renderer *renderer)
{
    debug_ui_image_release_all();
    if (renderer) {
        s_mode     = ImageMode::Sdl;
        s_renderer = renderer;
    }
}

void
debug_ui_image_use_dx12(struct render_backend *backend)
{
    debug_ui_image_release_all();
    if (backend) {
        s_mode    = ImageMode::Dx12;
        s_backend = backend;
    }
}

bool
debug_ui_image_available(void)
{
    return s_mode != ImageMode::None;
}

DebugUiImage::DebugUiImage()
{
    m_next   = s_images;
    s_images = this;
}

DebugUiImage::~DebugUiImage()
{
    release();
    for (DebugUiImage **link = &s_images; *link; link = &(*link)->m_next) {
        if (*link == this) {
            *link = m_next;
            break;
        }
    }
}

void
DebugUiImage::release()
{
    if (m_sdl) {
        SDL_DestroyTexture(m_sdl);
        m_sdl = nullptr;
    }
    if (m_native) {
        render_backend_texture_destroy(m_native);
        m_native = nullptr;
    }
    m_id     = 0;
    m_width  = 0;
    m_height = 0;
}

bool
DebugUiImage::ensure(int width, int height)
{
    if (width <= 0 || height <= 0) {
        return false;
    }
    if ((m_sdl || m_native) && m_width == width && m_height == height) {
        return true;
    }
    release();

    if (s_mode == ImageMode::Sdl && s_renderer) {
        m_sdl = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_ABGR8888,
                                  SDL_TEXTUREACCESS_STREAMING, width, height);
        if (!m_sdl) {
            return false;
        }
        SDL_SetTextureBlendMode(m_sdl, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(m_sdl, SDL_ScaleModeNearest); // crisp zoomed pixels
        m_id = (ImTextureID)(uintptr_t)m_sdl;
    }
#ifdef HAS_DX12
    else if (s_mode == ImageMode::Dx12 && s_backend) {
        // The SRV lands in the shared shader-visible heap, which is the heap the
        // debugger binds, so the GPU descriptor handle is directly usable as an
        // ImTextureID. (ImGui's own root signature carries a static LINEAR
        // sampler, so these are filtered rather than point-sampled when zoomed.)
        m_native = render_backend_texture_create(s_backend, RENDER_PIXEL_FORMAT_RGBA8888,
                                                 RENDER_TEXTURE_STREAMING,
                                                 RENDER_SCALE_NEAREST, width, height);
        if (!m_native) {
            return false;
        }
        D3D12_GPU_DESCRIPTOR_HANDLE handle = {};
        if (!render_dx12_texture_gpu_handle(m_native, &handle)) {
            release();
            return false;
        }
        m_id = (ImTextureID)handle.ptr;
    }
#endif
    else {
        return false;
    }

    m_width  = width;
    m_height = height;
    return true;
}

void
DebugUiImage::update(const uint32_t *pixels)
{
    if (!pixels) {
        return;
    }
    const int pitch = m_width * (int)sizeof(uint32_t);
    if (m_sdl) {
        SDL_UpdateTexture(m_sdl, nullptr, pixels, pitch);
    } else if (m_native) {
        render_backend_texture_update(m_native, nullptr, pixels, pitch);
    }
}

ImTextureID
DebugUiImage::id() const
{
    return m_id;
}
