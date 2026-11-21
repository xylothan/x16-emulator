// Commander X16 Emulator — reading a text layer as text

#include "screen_text.h"

#include <string.h>

#define NO_TEXT 0xFFFDu

static const char *const charset_names[] = { "petscii-upper", "petscii-lower", "iso" };

bool screen_charset_parse(const char *name, screen_charset_t *out) {
    for (int i = 0; i < 3; i++) {
        if (!strcmp(name, charset_names[i])) {
            *out = (screen_charset_t)i;
            return true;
        }
    }
    return false;
}

const char *screen_charset_name(screen_charset_t cs) {
    return charset_names[cs];
}

static int put_utf8(unsigned cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

// The half of the screen-code table the two PETSCII sets share. The top bit of a
// screen code is reverse video, so it is already gone.
static unsigned petscii_common(uint8_t code) {
    switch (code) {
    case 0x00: return '@';
    case 0x1B: return '[';
    case 0x1C: return 0x00A3;   // pound
    case 0x1D: return ']';
    case 0x1E: return 0x2191;   // up arrow
    case 0x1F: return 0x2190;   // left arrow
    default:   break;
    }
    if (code >= 0x20 && code <= 0x3F) return code;
    return NO_TEXT;
}

static unsigned iso_8859_15(uint8_t code) {
    switch (code) {
    case 0xA0: return ' ';      // no-break space, which reads as a space
    case 0xA4: return 0x20AC;
    case 0xA6: return 0x0160;
    case 0xA8: return 0x0161;
    case 0xB4: return 0x017D;
    case 0xB8: return 0x017E;
    case 0xBC: return 0x0152;
    case 0xBD: return 0x0153;
    case 0xBE: return 0x0178;
    default:   break;
    }
    if ((code >= 0x20 && code <= 0x7E) || code >= 0xA1) return code;
    return NO_TEXT;
}

int screen_text_glyph(uint8_t code, screen_charset_t cs, char out[3]) {
    unsigned cp;
    if (cs == SCREEN_CHARSET_ISO) {
        cp = iso_8859_15(code);
    } else {
        const uint8_t c = code & 0x7F;
        if (c >= 0x01 && c <= 0x1A) {
            cp = (cs == SCREEN_CHARSET_PETSCII_UPPER ? 'A' : 'a') + (c - 1);
        } else if (cs == SCREEN_CHARSET_PETSCII_LOWER && c >= 0x41 && c <= 0x5A) {
            cp = 'A' + (c - 0x41);
        } else {
            cp = petscii_common(c);
        }
    }
    return put_utf8(cp, out);
}

void screen_text_line(const screen_text_layout_t *l, int row, screen_charset_t cs,
                      uint8_t (*read_vram)(uint32_t address), char *out) {
    const int first_col = l->hscroll / l->tile_w;
    const int map_row   = (l->vscroll / l->tile_h + row) & (l->map_h - 1);
    const int columns   = l->columns < SCREEN_TEXT_MAX_COLUMNS ? l->columns : SCREEN_TEXT_MAX_COLUMNS;

    char *p = out, *end = out;
    for (int c = 0; c < columns; c++) {
        const int map_col = (first_col + c) & (l->map_w - 1);
        const uint32_t address = (l->map_base + 2u * (uint32_t)(map_row * l->map_w + map_col)) & 0x1FFFF;
        char glyph[3];
        const int n = screen_text_glyph(read_vram(address), cs, glyph);
        memcpy(p, glyph, (size_t)n);
        p += n;
        if (n != 1 || glyph[0] != ' ') end = p;
    }
    *end = '\0';
}
