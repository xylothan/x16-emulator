#ifndef X16_RENDER_BACKEND_H
#define X16_RENDER_BACKEND_H

#include <stdbool.h>
#include <stdint.h>

typedef struct SDL_Window SDL_Window;

typedef struct render_backend render_backend_t;
typedef struct render_texture render_texture_t;

typedef struct render_rect {
	int x;
	int y;
	int w;
	int h;
} render_rect_t;

typedef struct render_color {
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t a;
} render_color_t;

typedef enum render_pixel_format {
	RENDER_PIXEL_FORMAT_BGRA8888,
	RENDER_PIXEL_FORMAT_RGBA4444
} render_pixel_format_t;

typedef enum render_texture_access {
	RENDER_TEXTURE_STATIC,
	RENDER_TEXTURE_STREAMING
} render_texture_access_t;

typedef enum render_scale_quality {
	RENDER_SCALE_NEAREST,
	RENDER_SCALE_LINEAR,
	RENDER_SCALE_BEST
} render_scale_quality_t;

#ifdef __cplusplus
extern "C" {
#endif

render_backend_t *render_backend_create_sdl(SDL_Window *window);
#ifdef HAS_DX12
render_backend_t *render_backend_create_dx12(SDL_Window *window);
#endif
void render_backend_destroy(render_backend_t *backend);
const char *render_backend_name(render_backend_t *backend);
bool render_backend_is_healthy(render_backend_t *backend);

bool render_backend_resize(render_backend_t *backend, int width, int height);
bool render_backend_set_logical_size(render_backend_t *backend, int width, int height);

render_texture_t *render_backend_texture_create(render_backend_t *backend,
                                                render_pixel_format_t format,
                                                render_texture_access_t access,
                                                render_scale_quality_t quality,
                                                int width, int height);
void render_backend_texture_destroy(render_texture_t *texture);
bool render_backend_texture_update(render_texture_t *texture, const render_rect_t *rect,
                                   const void *pixels, int pitch);
bool render_backend_texture_set_color(render_texture_t *texture, render_color_t color);

bool render_backend_set_draw_color(render_backend_t *backend, render_color_t color);
bool render_backend_clear(render_backend_t *backend);
bool render_backend_copy(render_backend_t *backend, render_texture_t *texture,
                         const render_rect_t *source, const render_rect_t *destination);
bool render_backend_fill_rect(render_backend_t *backend, const render_rect_t *rect);
bool render_backend_draw_line(render_backend_t *backend, int x1, int y1, int x2, int y2);
void render_backend_present(render_backend_t *backend);

#ifdef __cplusplus
}
#endif

#endif
