# Raster-split decoding in the VERA debug views

## Status

**Not implemented.** The VERA panel's Tilemap and Bitmap views decode using the
layer registers as they are *right now*. That is correct for any program that
does not rewrite layer registers part-way down a frame, which is the large
majority, and it is what other emulator debuggers do.

This document exists because a first implementation was attempted, went through
nine corrected revisions, and was removed before merge. It records why, so the
next attempt does not repeat it.

## The problem

A program can rewrite VERA's layer registers mid-frame, normally from a line
IRQ, so that different horizontal bands of the screen are drawn with different
MAPBASE / TILEBASE / scroll values. This is a "raster split".

A debugger that reads the registers once therefore describes only the band that
happened to be active when it looked. Every other band decodes as garbage — not
subtly wrong, but visibly wrong, because MAPBASE points somewhere else entirely.

To decode correctly, a debug view needs to know, for each row of the layer
image, which register values produced it.

## Why the removed attempt did not work

The approach was to reconstruct that information *alongside* the renderer: an
array indexed by scanline, filled in from `render_line()`, mirroring whatever
the renderer was about to do.

It cannot be made reliable, because the renderer does not consume one coherent
set of registers. It consumes a mixture:

| Renderer | Layout registers | Row calculation | Palette offset |
|---|---|---|---|
| `render_layer_line_text` | `prev_layer_properties[1]` | `calc_layer_eff_y(props0, y)` — generation `[0]` | from `[1]` |
| `render_layer_line_tile` | `prev_layer_properties[1]` | `calc_layer_eff_y(props0, y)` — generation `[0]` | from `[1]` |
| `render_layer_line_bitmap` | `prev_layer_properties[1]` | `y % props1->tileh` — a different expression | **live** `reg_layer` |

`prev_layer_properties` is a two-stage delay (`video.c`, in the `y != y_prev`
block). So "the registers that rendered this line" is not a single generation,
is not even a single rule, and one of the inputs is not delayed at all.

Every revision of the mirrored approach was a further special case bolted on to
that reconstruction, and each one was found wrong by review:

1. Snapshot live `reg_layer` — wrong generation entirely.
2. Snapshot delayed generation `[1]` — right for layout, wrong for the row.
3. Record `calc_layer_eff_y(props0, ...)` — right for text/tile, wrong for bitmap.
4. Mirror the renderer's mode dispatch — but the block was duplicated rather
   than moved, so the stale copy won on warp-skipped frames.
5. Delete the duplicate.
6. Border scanlines were published as having displayed a layer, so a register
   change during the top border could beat the active line to a layer row.
7. Capture only intervals that draw pixels — `render_line()` runs several times
   per scanline and the trailing ones are horizontal blanking.
8. A "nothing displayed" interval settled the line, so enabling output mid-line
   was never recorded; and the tracker's statics survived reset and warp skips.
9. Still finalising on intervals that drew no *layer* output, and one settle
   flag shared by two independently-enabled layers.

The pattern is not bad luck. Reconstructing a decision from outside the code
that makes it means every branch in that code becomes a branch you must mirror,
and nothing tells you when you have missed one.

## The shape a correct implementation should have

Record the decode inputs **where the renderer uses them**, so they cannot
disagree with it by construction:

1. Have each `render_layer_line_*` publish the resolved state it actually used —
   map base, tile base, bpp, tile size, palette offset, and the layer row it
   indexed — rather than having a separate site guess at it.
2. Commit that record from the compositor, and only when it consumes a non-empty
   visible span for that layer. A layer function can run for a line whose output
   is never composited.
3. Track settlement **per layer**. The two layers are enabled independently and
   can change independently mid-line.
4. Decide explicitly what a mid-line change means. Either record `(x0, x1,
   state)` spans, or define "first visible span per layer wins" and flag lines
   that contained more than one state, so the view can say so rather than
   silently pick one.
5. Invalidate through one helper used by every path that discards the picture
   (reset, mode change, framebuffer clear), so validity and any capture-tracking
   state can never disagree.

## Testing

None of the current test executables exercises `video.c`. Every wrong revision
above built clean and passed all six suites; that was never evidence about this
feature. A real attempt needs targeted tests: a scripted register change at a
known scanline, then assertions about what the recorded state says for rows
above and below the split — including the bitmap path, a mid-line enable, a
reset, and a warp-skipped frame.

## What is lost meanwhile

Only correct decoding of programs that change layer registers mid-frame:
demos, and games with split-screen status bars. Static screens, which is nearly
everything else, decode correctly today. The Tilemap and Bitmap views say which
registers they used, so the view does not claim more than it knows.
