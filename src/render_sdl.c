#include "render_backend_internal.h"

#include <SDL.h>
#include <stdlib.h>

typedef struct sdl_backend {
	render_backend_t base;
	SDL_Renderer *renderer;
	char name[32];
} sdl_backend_t;

typedef struct sdl_texture {
	render_texture_t base;
	SDL_Texture *texture;
} sdl_texture_t;

static SDL_Rect
sdl_rect(const render_rect_t *rect)
{
	SDL_Rect result = { rect->x, rect->y, rect->w, rect->h };
	return result;
}

static Uint32
sdl_pixel_format(render_pixel_format_t format)
{
	switch (format) {
		case RENDER_PIXEL_FORMAT_BGRA8888:
			return SDL_PIXELFORMAT_RGB888;
		case RENDER_PIXEL_FORMAT_RGBA4444:
			return SDL_PIXELFORMAT_RGBA4444;
	}
	return SDL_PIXELFORMAT_UNKNOWN;
}

static int
sdl_texture_access(render_texture_access_t access)
{
	return access == RENDER_TEXTURE_STREAMING
		? SDL_TEXTUREACCESS_STREAMING
		: SDL_TEXTUREACCESS_STATIC;
}

static const char *
sdl_scale_quality(render_scale_quality_t quality)
{
	switch (quality) {
		case RENDER_SCALE_NEAREST:
			return "nearest";
		case RENDER_SCALE_LINEAR:
			return "linear";
		case RENDER_SCALE_BEST:
			return "best";
	}
	return "nearest";
}

static void
sdl_destroy(render_backend_t *backend)
{
	sdl_backend_t *sdl = (sdl_backend_t *)backend;
	SDL_DestroyRenderer(sdl->renderer);
	free(sdl);
}

static const char *
sdl_name(render_backend_t *backend)
{
	return ((sdl_backend_t *)backend)->name;
}

static bool
sdl_is_healthy(render_backend_t *backend)
{
	(void)backend;
	return true;
}

static bool
sdl_resize(render_backend_t *backend, int width, int height)
{
	(void)backend;
	(void)width;
	(void)height;
	return true;
}

static bool
sdl_set_logical_size(render_backend_t *backend, int width, int height)
{
	sdl_backend_t *sdl = (sdl_backend_t *)backend;
	return SDL_RenderSetLogicalSize(sdl->renderer, width, height) == 0;
}

static render_texture_t *
sdl_texture_create(render_backend_t *backend, render_pixel_format_t format,
                   render_texture_access_t access, render_scale_quality_t quality,
                   int width, int height)
{
	sdl_backend_t *sdl = (sdl_backend_t *)backend;
	sdl_texture_t *texture = malloc(sizeof(*texture));
	if (!texture) {
		SDL_OutOfMemory();
		return NULL;
	}

	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, sdl_scale_quality(quality));
	texture->texture = SDL_CreateTexture(sdl->renderer, sdl_pixel_format(format),
	                                     sdl_texture_access(access), width, height);
	if (!texture->texture) {
		free(texture);
		return NULL;
	}
	texture->base.backend = backend;
	return &texture->base;
}

static void
sdl_texture_destroy(render_texture_t *texture)
{
	sdl_texture_t *sdl = (sdl_texture_t *)texture;
	SDL_DestroyTexture(sdl->texture);
	free(sdl);
}

static bool
sdl_texture_update(render_texture_t *texture, const render_rect_t *rect,
                   const void *pixels, int pitch)
{
	sdl_texture_t *sdl = (sdl_texture_t *)texture;
	SDL_Rect converted;
	const SDL_Rect *sdl_rect_ptr = NULL;
	if (rect) {
		converted = sdl_rect(rect);
		sdl_rect_ptr = &converted;
	}
	return SDL_UpdateTexture(sdl->texture, sdl_rect_ptr, pixels, pitch) == 0;
}

static bool
sdl_texture_set_color(render_texture_t *texture, render_color_t color)
{
	sdl_texture_t *sdl = (sdl_texture_t *)texture;
	return SDL_SetTextureColorMod(sdl->texture, color.r, color.g, color.b) == 0;
}

static bool
sdl_set_draw_color(render_backend_t *backend, render_color_t color)
{
	sdl_backend_t *sdl = (sdl_backend_t *)backend;
	return SDL_SetRenderDrawColor(sdl->renderer, color.r, color.g, color.b, color.a) == 0;
}

static bool
sdl_clear(render_backend_t *backend)
{
	return SDL_RenderClear(((sdl_backend_t *)backend)->renderer) == 0;
}

static bool
sdl_copy(render_backend_t *backend, render_texture_t *texture,
         const render_rect_t *source, const render_rect_t *destination)
{
	sdl_backend_t *sdl = (sdl_backend_t *)backend;
	sdl_texture_t *sdl_texture = (sdl_texture_t *)texture;
	SDL_Rect converted_source;
	SDL_Rect converted_destination;
	const SDL_Rect *source_ptr = NULL;
	const SDL_Rect *destination_ptr = NULL;
	if (source) {
		converted_source = sdl_rect(source);
		source_ptr = &converted_source;
	}
	if (destination) {
		converted_destination = sdl_rect(destination);
		destination_ptr = &converted_destination;
	}
	return SDL_RenderCopy(sdl->renderer, sdl_texture->texture,
	                      source_ptr, destination_ptr) == 0;
}

static bool
sdl_fill_rect(render_backend_t *backend, const render_rect_t *rect)
{
	sdl_backend_t *sdl = (sdl_backend_t *)backend;
	SDL_Rect converted;
	const SDL_Rect *rect_ptr = NULL;
	if (rect) {
		converted = sdl_rect(rect);
		rect_ptr = &converted;
	}
	return SDL_RenderFillRect(sdl->renderer, rect_ptr) == 0;
}

static bool
sdl_draw_line(render_backend_t *backend, int x1, int y1, int x2, int y2)
{
	sdl_backend_t *sdl = (sdl_backend_t *)backend;
	return SDL_RenderDrawLine(sdl->renderer, x1, y1, x2, y2) == 0;
}

static void
sdl_present(render_backend_t *backend)
{
	SDL_RenderPresent(((sdl_backend_t *)backend)->renderer);
}

static const render_backend_ops_t sdl_ops = {
	sdl_destroy,
	sdl_name,
	sdl_is_healthy,
	sdl_resize,
	sdl_set_logical_size,
	sdl_texture_create,
	sdl_texture_destroy,
	sdl_texture_update,
	sdl_texture_set_color,
	sdl_set_draw_color,
	sdl_clear,
	sdl_copy,
	sdl_fill_rect,
	sdl_draw_line,
	sdl_present
};

render_backend_t *
render_backend_create_sdl(SDL_Window *window)
{
	sdl_backend_t *backend = calloc(1, sizeof(*backend));
	if (!backend) {
		SDL_OutOfMemory();
		return NULL;
	}

	backend->renderer = SDL_CreateRenderer(window, -1, 0);
	if (!backend->renderer) {
		free(backend);
		return NULL;
	}
	backend->base.ops = &sdl_ops;

	SDL_RendererInfo info;
	if (SDL_GetRendererInfo(backend->renderer, &info) == 0 && info.name) {
		SDL_strlcpy(backend->name, info.name, sizeof(backend->name));
	} else {
		SDL_strlcpy(backend->name, "unknown", sizeof(backend->name));
	}

	return &backend->base;
}
