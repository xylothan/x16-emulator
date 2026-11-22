// Commander X16 Emulator — reading a text layer as text
//
// Turns a tile map of screen codes into UTF-8 lines. No emulator state: the
// caller describes the layer and supplies a way to read VRAM, so the decoding
// can be tested on its own.

#ifndef _SCREEN_TEXT_H_
#define _SCREEN_TEXT_H_

#include <stdbool.h>
#include <stdint.h>

// A screen code means different things under different character sets, and
// nothing in VERA records which one the font in VRAM is.
typedef enum {
    SCREEN_CHARSET_PETSCII_UPPER,   // upper case and graphics, the boot default
    SCREEN_CHARSET_PETSCII_LOWER,   // lower and upper case
    SCREEN_CHARSET_ISO,             // ISO-8859-15
} screen_charset_t;

typedef struct {
    bool     enabled;           // the layer is on and video output is on
    bool     is_text;           // 1bpp tiles, so each cell is a screen code
    uint32_t map_base;
    int      map_w, map_h;      // in tiles
    int      tile_w, tile_h;    // in pixels
    int      hscroll, vscroll;  // in pixels
    int      columns, rows;     // whole tiles in the visible window
} screen_text_layout_t;

#define SCREEN_TEXT_MAX_COLUMNS 256
// Every cell can take three bytes of UTF-8.
#define SCREEN_TEXT_LINE_MAX (SCREEN_TEXT_MAX_COLUMNS * 3 + 1)

bool        screen_charset_parse(const char *name, screen_charset_t *out);
const char *screen_charset_name(screen_charset_t cs);

// One screen code as UTF-8, returning its length. A code with no text
// equivalent, such as a PETSCII graphic, becomes U+FFFD.
int screen_text_glyph(uint8_t code, screen_charset_t cs, char out[3]);

// One visible row, scrolled and wrapped as VERA would, with trailing spaces
// removed. `out` holds SCREEN_TEXT_LINE_MAX bytes.
void screen_text_line(const screen_text_layout_t *layout, int row, screen_charset_t cs,
                      uint8_t (*read_vram)(uint32_t address), char *out);

#endif
