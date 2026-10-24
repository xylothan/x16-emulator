#ifndef X16_RENDER_BACKEND_INTERNAL_H
#define X16_RENDER_BACKEND_INTERNAL_H

#include "render_backend.h"

typedef struct render_backend_ops {
	void (*destroy)(render_backend_t *backend);
	const char *(*name)(render_backend_t *backend);
	bool (*is_healthy)(render_backend_t *backend);
	bool (*resize)(render_backend_t *backend, int width, int height);
	bool (*set_logical_size)(render_backend_t *backend, int width, int height);
	render_texture_t *(*texture_create)(render_backend_t *backend,
	                                    render_pixel_format_t format,
	                                    render_texture_access_t access,
	                                    render_scale_quality_t quality,
	                                    int width, int height);
	void (*texture_destroy)(render_texture_t *texture);
	bool (*texture_update)(render_texture_t *texture, const render_rect_t *rect,
	                       const void *pixels, int pitch);
	bool (*texture_set_color)(render_texture_t *texture, render_color_t color);
	bool (*set_draw_color)(render_backend_t *backend, render_color_t color);
	bool (*clear)(render_backend_t *backend);
	bool (*copy)(render_backend_t *backend, render_texture_t *texture,
	             const render_rect_t *source, const render_rect_t *destination);
	bool (*fill_rect)(render_backend_t *backend, const render_rect_t *rect);
	bool (*draw_line)(render_backend_t *backend, int x1, int y1, int x2, int y2);
	void (*present)(render_backend_t *backend);
} render_backend_ops_t;

struct render_backend {
	const render_backend_ops_t *ops;
};

struct render_texture {
	render_backend_t *backend;
};

#endif
