// Checks for reading a text layer as text (src/screen_text.c).

#include "screen_text.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(bool cond, const char *what) {
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    } else {
        printf("ok  : %s\n", what);
    }
}

static bool glyph_is(uint8_t code, screen_charset_t cs, const char *want) {
    char g[4] = { 0 };
    const int n = screen_text_glyph(code, cs, g);
    return (size_t)n == strlen(want) && !memcmp(g, want, (size_t)n);
}

#define FFFD "\xEF\xBF\xBD"

static void test_petscii(void) {
    const screen_charset_t up = SCREEN_CHARSET_PETSCII_UPPER, lo = SCREEN_CHARSET_PETSCII_LOWER;
    check(glyph_is(0x00, up, "@"), "code 0 is @");
    check(glyph_is(0x01, up, "A") && glyph_is(0x1A, up, "Z"), "upper set: 1 to 26 are A to Z");
    check(glyph_is(0x01, lo, "a") && glyph_is(0x1A, lo, "z"), "lower set: 1 to 26 are a to z");
    check(glyph_is(0x41, lo, "A") && glyph_is(0x5A, lo, "Z"), "lower set: $41 to $5A are A to Z");
    check(glyph_is(0x41, up, FFFD), "upper set: $41 is a graphic");
    check(glyph_is(0x20, up, " ") && glyph_is(0x2E, up, ".") && glyph_is(0x39, up, "9"),
          "$20 to $3F match ASCII");
    check(glyph_is(0x1C, up, "\xC2\xA3"), "the pound sign");
    check(glyph_is(0x1E, up, "\xE2\x86\x91") && glyph_is(0x1F, up, "\xE2\x86\x90"), "the arrows");
    check(glyph_is(0x81, up, "A") && glyph_is(0xA0, up, " "), "reverse video is the same character");
    check(glyph_is(0x7F, up, FFFD), "a graphic has no text");
}

static void test_iso(void) {
    const screen_charset_t iso = SCREEN_CHARSET_ISO;
    check(glyph_is(0x41, iso, "A") && glyph_is(0x61, iso, "a"), "ISO letters");
    check(glyph_is(0x01, iso, FFFD) && glyph_is(0x7F, iso, FFFD) && glyph_is(0x9F, iso, FFFD),
          "control codes have no text");
    check(glyph_is(0xA4, iso, "\xE2\x82\xAC"), "$A4 is the euro in 8859-15");
    check(glyph_is(0xBD, iso, "\xC5\x93"), "$BD is oe in 8859-15");
    check(glyph_is(0xE9, iso, "\xC3\xA9"), "$E9 is e-acute");
    check(glyph_is(0xA0, iso, " "), "a no-break space reads as a space");
}

static void test_names(void) {
    screen_charset_t cs;
    check(screen_charset_parse("iso", &cs) && cs == SCREEN_CHARSET_ISO, "parses iso");
    check(screen_charset_parse("petscii-lower", &cs) && cs == SCREEN_CHARSET_PETSCII_LOWER, "parses lower");
    check(!screen_charset_parse("ascii", &cs), "refuses an unknown name");
    check(!strcmp(screen_charset_name(SCREEN_CHARSET_PETSCII_UPPER), "petscii-upper"), "names a set");
}

// A 32 by 32 map at $1000, each cell a code and a colour byte.
static uint8_t vram[0x20000];

static uint8_t read_vram(uint32_t address) {
    return vram[address];
}

static void put(int col, int row, uint8_t code) {
    vram[0x1000 + 2 * (row * 32 + col)]     = code;
    vram[0x1000 + 2 * (row * 32 + col) + 1] = 0x61;  // must never be read as text
}

static void test_lines(void) {
    for (int r = 0; r < 32; r++) {
        for (int c = 0; c < 32; c++) put(c, r, 0x20);
    }
    put(0, 0, 0x08); put(1, 0, 0x09);        // HI
    put(31, 1, 0x01); put(0, 1, 0x02);       // A at the last column, B at the first
    put(4, 31, 0x03); put(4, 0, 0x04);       // C on the last row, D on the first

    screen_text_layout_t l;
    memset(&l, 0, sizeof l);
    l.map_base = 0x1000;
    l.map_w = l.map_h = 32;
    l.tile_w = l.tile_h = 8;
    l.columns = 10;
    l.rows = 4;

    char line[SCREEN_TEXT_LINE_MAX];
    screen_text_line(&l, 0, SCREEN_CHARSET_PETSCII_UPPER, read_vram, line);
    check(!strcmp(line, "HI  D"), "a row, without its trailing spaces");
    screen_text_line(&l, 2, SCREEN_CHARSET_PETSCII_UPPER, read_vram, line);
    check(line[0] == '\0', "a blank row is empty");

    l.hscroll = 31 * 8;
    screen_text_line(&l, 1, SCREEN_CHARSET_PETSCII_UPPER, read_vram, line);
    check(!strcmp(line, "AB"), "horizontal scroll wraps around the map");

    l.hscroll = 0;
    l.vscroll = 31 * 8 + 3;  // part way through a tile rounds down
    screen_text_line(&l, 0, SCREEN_CHARSET_PETSCII_UPPER, read_vram, line);
    check(!strcmp(line, "    C"), "vertical scroll picks the row");
    screen_text_line(&l, 1, SCREEN_CHARSET_PETSCII_UPPER, read_vram, line);
    check(!strcmp(line, "HI  D"), "vertical scroll wraps around the map");

    l.vscroll = 0;
    l.tile_w = 16;
    l.hscroll = 16;
    screen_text_line(&l, 0, SCREEN_CHARSET_PETSCII_UPPER, read_vram, line);
    check(!strcmp(line, "I  D"), "scroll is counted in the tile width");

    // Every cell at the widest map and the longest glyph has to fit.
    l.tile_w = 8;
    l.hscroll = 0;
    l.map_w = 256;
    l.map_h = 2;
    l.columns = 400;
    memset(&vram[0x1000], 0x7F, 2 * 256);
    screen_text_line(&l, 0, SCREEN_CHARSET_PETSCII_UPPER, read_vram, line);
    check(strlen(line) == 256 * 3, "a full row of replacement characters fits");
}

int main(void) {
    test_petscii();
    test_iso();
    test_names();
    test_lines();
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
