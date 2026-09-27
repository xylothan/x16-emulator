
#ifndef _RENDERTEXT_H
#define _RENDERTEXT_H

#include <SDL.h>
#include "render_backend.h"

#define CHAR_SCALE 		(1)										// character pixel size.

extern int xPos;
extern int yPos;

void DEBUGInitChars(render_backend_t *renderer);
void DEBUGFreeChars(void);
void DEBUGWrite(render_backend_t *renderer, int x, int y, int ch, SDL_Color colour);
void DEBUGString(render_backend_t *renderer, int x, int y, char *s, SDL_Color colour);
char *ltrim(char *s);

#endif
