#include "render_backend_internal.h"

#include <stddef.h>

void
render_backend_destroy(render_backend_t *backend)
{
	if (backend) {
		backend->ops->destroy(backend);
	}
}

const char *
render_backend_name(render_backend_t *backend)
{
	return backend ? backend->ops->name(backend) : NULL;
}

bool
render_backend_resize(render_backend_t *backend, int width, int height)
{
	return backend && backend->ops->resize(backend, width, height);
}

bool
render_backend_set_logical_size(render_backend_t *backend, int width, int height)
{
	return backend && backend->ops->set_logical_size(backend, width, height);
}

render_texture_t *
render_backend_texture_create(render_backend_t *backend, render_pixel_format_t format,
                              render_texture_access_t access,
                              render_scale_quality_t quality, int width, int height)
{
	if (!backend) {
		return NULL;
	}
	return backend->ops->texture_create(backend, format, access, quality, width, height);
}

void
render_backend_texture_destroy(render_texture_t *texture)
{
	if (texture) {
		texture->backend->ops->texture_destroy(texture);
	}
}

bool
render_backend_texture_update(render_texture_t *texture, const render_rect_t *rect,
                              const void *pixels, int pitch)
{
	return texture && texture->backend->ops->texture_update(texture, rect, pixels, pitch);
}

bool
render_backend_texture_set_color(render_texture_t *texture, render_color_t color)
{
	return texture && texture->backend->ops->texture_set_color(texture, color);
}

bool
render_backend_set_draw_color(render_backend_t *backend, render_color_t color)
{
	return backend && backend->ops->set_draw_color(backend, color);
}

bool
render_backend_clear(render_backend_t *backend)
{
	return backend && backend->ops->clear(backend);
}

bool
render_backend_copy(render_backend_t *backend, render_texture_t *texture,
                    const render_rect_t *source, const render_rect_t *destination)
{
	return backend && texture && texture->backend == backend &&
	       backend->ops->copy(backend, texture, source, destination);
}

bool
render_backend_fill_rect(render_backend_t *backend, const render_rect_t *rect)
{
	return backend && backend->ops->fill_rect(backend, rect);
}

bool
render_backend_draw_line(render_backend_t *backend, int x1, int y1, int x2, int y2)
{
	return backend && backend->ops->draw_line(backend, x1, y1, x2, y2);
}

void
render_backend_present(render_backend_t *backend)
{
	if (backend) {
		backend->ops->present(backend);
	}
}
