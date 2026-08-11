# Raster-split decoding in the VERA debug views

## What ships today

The VERA panel's Tilemap and Bitmap views decode each row of the layer image
with the layer registers that were in effect on the scanline that displayed it,
rather than with whatever the registers happen to say when the debugger looks.
Enable it with the **Follow raster** checkbox on those tabs (on by default).

`src/video.c` keeps a per-scanline record: the layer registers on the same
two-stage delay the renderers consume, the composer's effective Y, the layer row
the line displayed, and whether the layer actually reached the screen.
`video_get_layer_line_state()` exposes it, and `build_raster_row_regs()` in
`src/debug_ui/panels/vera_panel.cpp` indexes those snapshots by layer row.

This is correct for the case it exists for: a program rewriting MAPBASE /
TILEBASE / scroll part-way down a frame from a line IRQ, so that different
horizontal bands use different values — a split-screen status bar, or a demo
effect. Without it, a debugger reading the registers once describes only the
band that happened to be active when it looked, and every other band decodes as
visible garbage, because MAPBASE points somewhere else entirely.

## Known limits of the current implementation

These are accuracy limits, not failures. In each case the view is right for the
common shape of the effect and can be off in a narrower one. They are listed so
nobody has to rediscover them.

**1. VSCROLL and layout come from different register generations.**
The renderers take the layout from `prev_layer_properties[1]` but compute the
row with `calc_layer_eff_y(props0, ...)` — generation `[0]`. The snapshot
records the row the renderer computed, so text and tile modes land on the right
row; but the seven register bytes handed to the panel are generation `[1]`, so
anything the panel derives from them directly is a generation behind. Visible
only if scroll changes at the split.

**2. Bitmap mode is approximated.**
`render_layer_line_bitmap()` indexes with `eff_y % tileh` from generation `[1]`,
and takes its palette offset from the **live** `reg_layer` rather than either
delayed copy. The snapshot records `eff_y` as the row (VERA forces scroll to 0
in bitmap mode, so this matches in practice) and the delayed palette nibble, so
a palette-offset change at a raster split is reported up to two lines late.

**3. One snapshot per scanline, and a mid-line change is not represented.**
`render_line()` runs several times per scanline, once per compositor interval,
and the last call wins — including intervals covering horizontal blanking, which
draw nothing but may carry register writes made after the visible part of the
line was drawn. Enabling output, or a layer, part-way along a line is therefore
not represented faithfully.

**4. One settle point for two layers.**
The two layers are enabled independently and can change independently mid-line;
the record treats the line as a unit.

**5. Geometry changes are declined rather than followed.**
`same_layer_geometry()` refuses to substitute a snapshot whose tile geometry
differs from the live registers, because that would change the texture layout
mid-image. A split that changes colour depth or map size falls back to live
registers for those rows.

## What "exact" would require

The current design reconstructs, from `render_line()`, a decision the renderer
makes across three functions that each consume a different mixture of two
delayed register generations and — in bitmap mode — the live registers. Every
limit above is a consequence of mirroring that from outside rather than
recording it from inside.

An exact implementation should instead:

1. Have each `render_layer_line_*` publish the resolved state it actually used —
   map base, tile base, bpp, tile size, palette offset, and the layer row it
   indexed — so the record cannot disagree with the renderer by construction.
2. Commit that record from the compositor, and only when it consumes a non-empty
   visible span for that layer. A layer function can run for a line whose output
   is never composited.
3. Track settlement **per layer**, not per line.
4. Decide explicitly what a mid-line change means: either record `(x0, x1,
   state)` spans, or define "first visible span per layer wins" and flag lines
   that contained more than one state, so the view can say so rather than
   silently pick one.
5. Invalidate through one helper used by every path that discards the picture
   (reset, mode change, framebuffer clear), so validity and capture state can
   never disagree.
6. Handle a geometry change at a split by rendering the affected bands as
   separate images, instead of declining the substitution.

## Testing

**No current test executable exercises `src/video.c`.** During development every
revision of this feature — including several that were wrong — built clean and
passed all six ctest suites, so those suites say nothing about it either way.

An exact implementation needs targeted tests: script a register change at a
known scanline, then assert what the recorded state reports for rows above and
below the split. Cover the bitmap path, a mid-line enable, a geometry change, a
machine reset, a warp-skipped frame, and progressive mode.

---

## Follow-up: exact raster-split decoding

Tracked here, locally. Not an issue on any tracker.

The design an exact implementation needs is in the "What exact would require"
section above. Summary of the work:

1. Each `render_layer_line_*` publishes the resolved state it actually used
   (map base, tile base, bpp, tile size, palette offset, layer row indexed), so
   the record cannot disagree with the renderer by construction.
2. The compositor commits that record, and only when it consumes a non-empty
   visible span for that layer.
3. Settlement tracked per layer, not per line.
4. Mid-line changes represented explicitly: `(x0, x1, state)` spans, or
   "first visible span per layer wins" with a flag on lines carrying more than
   one state.
5. One invalidation helper shared by reset, mode change and framebuffer clear.
6. Geometry changes at a split rendered as separate bands rather than declined.

Do the tests first — nothing currently exercises `src/video.c`.
