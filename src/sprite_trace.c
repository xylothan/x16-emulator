// Commander X16 Emulator — sprite multiplexing trace. See sprite_trace.h for
// the data model and why the recording is split into a renderer stream and a
// mutation stream.

#include "sprite_trace.h"

#include <stdlib.h>
#include <string.h>

#define LINE_SLOTS (SPRITE_TRACE_LINES * SPRITE_TRACE_SLOTS)

// Generations start small and double. A frame using every slot without any
// multiplexing needs 128; a 1024-sprite multiplexer needs a little over that
// many. The ceiling exists so a runaway program cannot grow the trace without
// bound -- past it, further values in a slot are folded into the last
// generation, which understates the count rather than exhausting memory.
#define GEN_CAP_INITIAL 512
#define GEN_CAP_MAX     8192

// Attribute writes kept per frame. A multiplexer rewriting 8 bytes for each of
// 1024 sprites lands 8192, so this leaves headroom. On overflow later writes
// are counted but not stored: keeping an unbroken prefix means the burst
// analysis stays valid for what was kept, which a wrapping ring would not.
#define MUTATION_CAP 16384

#define DEFAULT_BURST_GAP 128

typedef struct {
	sprite_gen_t      *gens;
	uint16_t           gen_count;
	uint16_t           gen_cap;

	sprite_mutation_t *mutations;
	uint32_t           mutation_count;
	uint32_t           mutation_dropped;

	sprite_line_stat_t lines[SPRITE_TRACE_LINES];

	uint16_t          *line_gen;    // [LINE_SLOTS] index into gens, or SPRITE_TRACE_NO_GEN
	uint8_t           *line_flags;  // [LINE_SLOTS] SPRITE_TRACE_*
	uint16_t          *line_budget; // [LINE_SLOTS] sprite-fetch cycles

	sprite_frame_summary_t summary;
} trace_buffer_t;

bool sprite_trace_active = false;

static trace_buffer_t  buffers[2];
static trace_buffer_t *recording = &buffers[0];
static trace_buffer_t *published = &buffers[1];
static bool            have_published = false;
static bool            allocated = false;

// Per-slot recording state, valid only while a frame is in progress.
static uint16_t cur_gen[SPRITE_TRACE_SLOTS];
static bool     slot_dirty[SPRITE_TRACE_SLOTS];
static uint16_t slot_dirty_line[SPRITE_TRACE_SLOTS];
static uint32_t slot_dirty_cycle[SPRITE_TRACE_SLOTS];
static uint16_t slot_dirty_writes[SPRITE_TRACE_SLOTS];

static uint32_t frame_start_cycle;
static uint32_t frame_number;
static uint32_t burst_gap = DEFAULT_BURST_GAP;

// For attributing CPU cycles to the scanline the beam was on: line_begin for
// line N+1 tells us how many cycles elapsed since line N started.
static uint16_t prev_begin_line  = SPRITE_TRACE_NO_LINE;
static uint32_t prev_begin_cycle;

static sprite_trace_frame_t public_view;

static void
buffer_free(trace_buffer_t *b)
{
	free(b->gens);
	free(b->mutations);
	free(b->line_gen);
	free(b->line_flags);
	free(b->line_budget);
	memset(b, 0, sizeof(*b));
}

static bool
buffer_alloc(trace_buffer_t *b)
{
	memset(b, 0, sizeof(*b));
	b->gen_cap     = GEN_CAP_INITIAL;
	b->gens        = (sprite_gen_t *)malloc(sizeof(sprite_gen_t) * b->gen_cap);
	b->mutations   = (sprite_mutation_t *)malloc(sizeof(sprite_mutation_t) * MUTATION_CAP);
	b->line_gen    = (uint16_t *)malloc(sizeof(uint16_t) * LINE_SLOTS);
	b->line_flags  = (uint8_t *)malloc(LINE_SLOTS);
	b->line_budget = (uint16_t *)malloc(sizeof(uint16_t) * LINE_SLOTS);
	if (!b->gens || !b->mutations || !b->line_gen || !b->line_flags || !b->line_budget) {
		buffer_free(b);
		return false;
	}
	return true;
}

// Clear a buffer for a new frame. The residency map is reset to "the renderer
// never reached this slot" rather than zero, because slot 0 is a real index.
static void
buffer_begin(trace_buffer_t *b)
{
	b->gen_count        = 0;
	b->mutation_count   = 0;
	b->mutation_dropped = 0;
	memset(&b->summary, 0, sizeof(b->summary));
	memset(b->lines, 0, sizeof(b->lines));
	memset(b->line_flags, 0, LINE_SLOTS);
	memset(b->line_budget, 0, sizeof(uint16_t) * LINE_SLOTS);
	for (int i = 0; i < LINE_SLOTS; i++) {
		b->line_gen[i] = SPRITE_TRACE_NO_GEN;
	}
	for (int i = 0; i < SPRITE_TRACE_LINES; i++) {
		b->lines[i].cut_slot = SPRITE_TRACE_NO_LINE;
	}
}

size_t
sprite_trace_memory_usage(void)
{
	if (!allocated) {
		return 0;
	}
	size_t per = sizeof(sprite_mutation_t) * MUTATION_CAP + sizeof(uint16_t) * LINE_SLOTS +
	             (size_t)LINE_SLOTS + sizeof(uint16_t) * LINE_SLOTS + sizeof(((trace_buffer_t *)0)->lines);
	return 2 * per + sizeof(sprite_gen_t) * (buffers[0].gen_cap + buffers[1].gen_cap);
}

void
sprite_trace_reset(void)
{
	have_published    = false;
	prev_begin_line   = SPRITE_TRACE_NO_LINE;
	frame_start_cycle = 0;
	frame_number      = 0;
	memset(slot_dirty, 0, sizeof(slot_dirty));
	memset(slot_dirty_writes, 0, sizeof(slot_dirty_writes));
	for (int i = 0; i < SPRITE_TRACE_SLOTS; i++) {
		cur_gen[i] = SPRITE_TRACE_NO_GEN;
	}
	if (allocated) {
		buffer_begin(recording);
		buffer_begin(published);
	}
}

bool
sprite_trace_set_enabled(bool on)
{
	if (on == sprite_trace_active) {
		return true;
	}
	if (on) {
		if (!allocated) {
			if (!buffer_alloc(&buffers[0])) {
				return false;
			}
			if (!buffer_alloc(&buffers[1])) {
				buffer_free(&buffers[0]);
				return false;
			}
			allocated = true;
		}
		recording = &buffers[0];
		published = &buffers[1];
		sprite_trace_reset();
		sprite_trace_active = true;
	} else {
		sprite_trace_active = false;
		have_published      = false;
		if (allocated) {
			buffer_free(&buffers[0]);
			buffer_free(&buffers[1]);
			allocated = false;
		}
	}
	return true;
}

void
sprite_trace_set_burst_gap(uint32_t cycles)
{
	burst_gap = cycles;
}

uint32_t
sprite_trace_get_burst_gap(void)
{
	return burst_gap;
}

// Decode the attribute bytes the way refresh_sprite_properties() does, so a
// generation carries the same interpretation the renderer used.
static void
decode_attr(sprite_gen_t *g, const uint8_t attr[8])
{
	memcpy(g->attr, attr, 8);

	g->zdepth         = (uint8_t)((attr[6] >> 2) & 3);
	g->collision_mask = (uint8_t)(attr[6] & 0xf0);
	g->hflip          = (attr[6] & 1) != 0;
	g->vflip          = ((attr[6] >> 1) & 1) != 0;

	g->x = (int16_t)(attr[2] | ((attr[3] & 3) << 8));
	g->y = (int16_t)(attr[4] | ((attr[5] & 3) << 8));

	g->width  = (uint16_t)(1u << ((((attr[7] >> 4) & 3) + 3)));
	g->height = (uint16_t)(1u << ((attr[7] >> 6) + 3));

	if (g->x >= (int16_t)(0x400 - g->width)) {
		g->x = (int16_t)(g->x - 0x400);
	}
	if (g->y >= (int16_t)(0x400 - g->height)) {
		g->y = (int16_t)(g->y - 0x400);
	}

	g->color_mode     = (uint8_t)((attr[1] >> 7) & 1);
	g->address        = (uint32_t)(attr[0] << 5 | (attr[1] & 0xf) << 13);
	g->palette_offset = (uint8_t)((attr[7] & 0x0f) << 4);
}

// Open a generation for a slot whose value the CPU changed. Returns the index,
// or the slot's existing generation if the table is full.
static uint16_t
open_gen(trace_buffer_t *b, uint8_t slot, const uint8_t attr[8])
{
	if (b->gen_count >= b->gen_cap) {
		if (b->gen_cap >= GEN_CAP_MAX) {
			return cur_gen[slot];
		}
		uint16_t     ncap = (uint16_t)(b->gen_cap * 2 > GEN_CAP_MAX ? GEN_CAP_MAX : b->gen_cap * 2);
		sprite_gen_t *ng  = (sprite_gen_t *)realloc(b->gens, sizeof(sprite_gen_t) * ncap);
		if (!ng) {
			return cur_gen[slot];
		}
		b->gens    = ng;
		b->gen_cap = ncap;
	}

	uint16_t      idx = b->gen_count++;
	sprite_gen_t *g   = &b->gens[idx];
	memset(g, 0, sizeof(*g));
	decode_attr(g, attr);
	g->slot       = slot;
	g->first_line = SPRITE_TRACE_NO_LINE;
	g->last_line  = SPRITE_TRACE_NO_LINE;

	uint16_t prev = cur_gen[slot];
	g->index_in_slot = (uint16_t)(prev == SPRITE_TRACE_NO_GEN ? 0 : b->gens[prev].index_in_slot + 1);

	return idx;
}

void
sprite_trace_note_write(uint8_t slot, uint8_t offset, uint8_t old_val, uint8_t new_val,
                        uint16_t line, uint32_t cpu_cycle)
{
	if (!sprite_trace_active || !allocated) {
		return;
	}
	slot &= (SPRITE_TRACE_SLOTS - 1);
	offset &= 7;

	trace_buffer_t *b = recording;

	b->summary.mutations++;
	if (line < SPRITE_TRACE_LINES) {
		b->summary.mutations_onscreen++;
	}

	if (b->mutation_count < MUTATION_CAP) {
		sprite_mutation_t *m = &b->mutations[b->mutation_count++];
		m->cycle             = cpu_cycle;
		m->line              = line;
		m->slot              = slot;
		m->offset            = offset;
		m->old_val           = old_val;
		m->new_val           = new_val;
	} else {
		b->mutation_dropped++;
	}

	// A write that stores the value already there costs the CPU exactly as much
	// as any other and belongs in the cost total, but it does not start a new
	// generation: nothing about what the beam will draw has changed.
	if (old_val == new_val) {
		return;
	}

	if (!slot_dirty[slot]) {
		slot_dirty[slot]        = true;
		slot_dirty_writes[slot] = 0;
	}
	// Kept as the most recent write, so the generation is stamped with the
	// moment its value became complete rather than when the update began. That
	// is the instant a late write has to be judged against.
	slot_dirty_line[slot]  = line;
	slot_dirty_cycle[slot] = cpu_cycle;
	slot_dirty_writes[slot]++;
}

void
sprite_trace_line_begin(uint16_t line, uint32_t cpu_cycle)
{
	if (!sprite_trace_active || !allocated) {
		return;
	}

	// Close out the previous line's CPU accounting before touching this one.
	// Accumulated rather than assigned: a line re-rendered by a mid-line split
	// gets begun twice, and both spans were spent with the beam on that line.
	if (prev_begin_line < SPRITE_TRACE_LINES) {
		recording->lines[prev_begin_line].cpu_cycles += cpu_cycle - prev_begin_cycle;
	}
	prev_begin_line  = line;
	prev_begin_cycle = cpu_cycle;

	if (line >= SPRITE_TRACE_LINES) {
		return;
	}

	trace_buffer_t *b   = recording;
	const size_t    off = (size_t)line * SPRITE_TRACE_SLOTS;
	for (int s = 0; s < SPRITE_TRACE_SLOTS; s++) {
		b->line_gen[off + s] = SPRITE_TRACE_NO_GEN;
	}
	memset(b->line_flags + off, 0, SPRITE_TRACE_SLOTS);
	memset(b->line_budget + off, 0, sizeof(uint16_t) * SPRITE_TRACE_SLOTS);

	uint32_t keep_cycles = b->lines[line].cpu_cycles;
	memset(&b->lines[line], 0, sizeof(b->lines[line]));
	b->lines[line].cpu_cycles = keep_cycles;
	b->lines[line].cut_slot   = SPRITE_TRACE_NO_LINE;
	b->lines[line].rendered   = true;
}

void
sprite_trace_note_slot(uint16_t line, uint8_t slot, const uint8_t attr[8],
                       uint16_t budget_used, uint8_t flags)
{
	if (!sprite_trace_active || !allocated || line >= SPRITE_TRACE_LINES) {
		return;
	}
	slot &= (SPRITE_TRACE_SLOTS - 1);

	trace_buffer_t *b = recording;

	// The renderer is reading this slot now, so now is when a pending change
	// becomes the value that was actually displayed -- half-written included.
	if (slot_dirty[slot]) {
		uint16_t idx = open_gen(b, slot, attr);
		if (idx != SPRITE_TRACE_NO_GEN) {
			sprite_gen_t *g = &b->gens[idx];
			if (idx == b->gen_count - 1) {
				g->write_line  = slot_dirty_line[slot];
				g->write_cycle = slot_dirty_cycle[slot];
				g->writes      = slot_dirty_writes[slot];
			} else {
				// Folded into the previous generation because the table is
				// full; keep its write count honest.
				g->writes = (uint16_t)(g->writes + slot_dirty_writes[slot]);
			}
			cur_gen[slot] = idx;
		}
		slot_dirty[slot]        = false;
		slot_dirty_writes[slot] = 0;
	}

	const size_t i  = (size_t)line * SPRITE_TRACE_SLOTS + slot;
	b->line_gen[i]  = cur_gen[slot];
	b->line_flags[i] = flags;
	b->line_budget[i] = budget_used;
}

void
sprite_trace_line_end(uint16_t line, uint16_t budget_used, bool exhausted, uint16_t cut_slot,
                      uint16_t evaluated, uint16_t drawn)
{
	if (!sprite_trace_active || !allocated || line >= SPRITE_TRACE_LINES) {
		return;
	}
	sprite_line_stat_t *st = &recording->lines[line];
	st->budget_used        = budget_used;
	st->exhausted          = exhausted;
	st->cut_slot           = cut_slot;
	st->evaluated          = evaluated;
	st->drawn              = drawn;
	st->rendered           = true;
}

// Walk the finished residency map once and turn it into per-generation totals
// and a frame summary. Done here rather than incrementally because a scanline
// can be rendered more than once and only the final pass counts.
static void
finalize(trace_buffer_t *b, uint32_t end_cycle)
{
	sprite_frame_summary_t *s = &b->summary;
	s->frame                  = frame_number;
	s->cpu_cycles             = end_cycle - frame_start_cycle;
	s->gen_count              = b->gen_count;
	s->burst_gap              = burst_gap;

	for (uint16_t line = 0; line < SPRITE_TRACE_LINES; line++) {
		const size_t off = (size_t)line * SPRITE_TRACE_SLOTS;
		for (int slot = 0; slot < SPRITE_TRACE_SLOTS; slot++) {
			uint16_t idx = b->line_gen[off + slot];
			if (idx == SPRITE_TRACE_NO_GEN || idx >= b->gen_count) {
				continue;
			}
			sprite_gen_t *g = &b->gens[idx];
			uint8_t       f = b->line_flags[off + slot];

			g->lines_resident++;
			g->budget_cycles += b->line_budget[off + slot];
			if (f & SPRITE_TRACE_CUT) {
				g->flags |= SPRITE_GEN_CUT;
			}
			if (f & SPRITE_TRACE_DREW) {
				g->flags |= SPRITE_GEN_DREW;
				g->lines_drawn++;
				if (g->first_line == SPRITE_TRACE_NO_LINE) {
					g->first_line = line;
				}
				g->last_line = line;
			}
		}
	}

	uint16_t drawn_gens_in_slot[SPRITE_TRACE_SLOTS];
	uint16_t gens_in_slot[SPRITE_TRACE_SLOTS];
	memset(drawn_gens_in_slot, 0, sizeof(drawn_gens_in_slot));
	memset(gens_in_slot, 0, sizeof(gens_in_slot));

	for (uint16_t i = 0; i < b->gen_count; i++) {
		sprite_gen_t *g = &b->gens[i];
		gens_in_slot[g->slot]++;

		if (g->flags & SPRITE_GEN_DREW) {
			s->effective_sprites++;
			drawn_gens_in_slot[g->slot]++;
		} else if (!(g->flags & SPRITE_GEN_CARRIED)) {
			s->phantom_writes++;
		}

		// Late: the value only became complete after the beam had already
		// passed the whole Y span it was meant to appear in, so nothing this
		// frame could ever have drawn it. Computed in int, because a sprite
		// parked just above the screen has a negative Y and its span ends at or
		// before line 0 -- which is late for every write, and would wrap to a
		// huge number if this were done in uint16_t.
		const int y_end = (int)g->y + (int)g->height;
		if (!(g->flags & SPRITE_GEN_CARRIED) && g->write_line < SPRITE_TRACE_LINES &&
		    g->zdepth != 0 && (int)g->write_line >= y_end) {
			g->flags |= SPRITE_GEN_LATE;
			s->late_writes++;
		}
	}

	for (int slot = 0; slot < SPRITE_TRACE_SLOTS; slot++) {
		if (drawn_gens_in_slot[slot] > 0) {
			s->slots_used++;
		}
		if (drawn_gens_in_slot[slot] > 1) {
			s->slots_multiplexed++;
		}
		if (gens_in_slot[slot] > s->max_gens_in_slot) {
			s->max_gens_in_slot = gens_in_slot[slot];
		}
	}
	s->multiplexing = s->slots_multiplexed > 0;

	for (uint16_t line = 0; line < SPRITE_TRACE_LINES; line++) {
		const sprite_line_stat_t *st = &b->lines[line];
		s->budget_used_total += st->budget_used;
		if (st->exhausted) {
			s->lines_exhausted++;
		}
		if (st->budget_used > s->peak_budget) {
			s->peak_budget = st->budget_used;
			s->peak_line   = line;
		}
	}

	// Cluster the attribute writes into bursts. One burst is one visit to the
	// multiplexer's raster handler; the cycles it spans are what the
	// multiplexing costs the guest. It is a lower bound -- the handler's entry,
	// exit and any work after its final store fall outside the span.
	uint32_t burst_start = 0;
	uint32_t prev_cycle  = 0;
	bool     in_burst    = false;
	for (uint32_t i = 0; i < b->mutation_count; i++) {
		uint32_t c = b->mutations[i].cycle;
		if (!in_burst || c - prev_cycle > burst_gap) {
			if (in_burst) {
				s->mutation_cycles += prev_cycle - burst_start;
			}
			s->mutation_bursts++;
			burst_start = c;
			in_burst    = true;
		}
		prev_cycle = c;
	}
	if (in_burst) {
		s->mutation_cycles += prev_cycle - burst_start;
	}
}

void
sprite_trace_frame_advance(uint32_t frame, uint32_t cpu_cycle,
                           const uint8_t attrs[SPRITE_TRACE_SLOTS][8])
{
	if (!sprite_trace_active || !allocated) {
		return;
	}

	// Charge the final scanline before the totals are taken.
	if (prev_begin_line < SPRITE_TRACE_LINES) {
		recording->lines[prev_begin_line].cpu_cycles += cpu_cycle - prev_begin_cycle;
	}
	prev_begin_line = SPRITE_TRACE_NO_LINE;

	finalize(recording, cpu_cycle);

	trace_buffer_t *done = recording;
	recording            = published;
	published            = done;
	have_published       = true;

	frame_number      = frame;
	frame_start_cycle = cpu_cycle;
	buffer_begin(recording);

	// Seed each slot with the value it starts the frame holding, so a sprite
	// that is never rewritten still has a generation to be recorded against.
	memset(slot_dirty, 0, sizeof(slot_dirty));
	memset(slot_dirty_writes, 0, sizeof(slot_dirty_writes));
	for (int slot = 0; slot < SPRITE_TRACE_SLOTS; slot++) {
		cur_gen[slot] = SPRITE_TRACE_NO_GEN;
		uint16_t idx  = open_gen(recording, (uint8_t)slot, attrs[slot]);
		if (idx != SPRITE_TRACE_NO_GEN) {
			recording->gens[idx].write_line  = SPRITE_TRACE_NO_LINE;
			recording->gens[idx].write_cycle = cpu_cycle;
			recording->gens[idx].writes      = 0;
			recording->gens[idx].flags |= SPRITE_GEN_CARRIED;
			cur_gen[slot] = idx;
		}
	}
}

const sprite_trace_frame_t *
sprite_trace_last_frame(void)
{
	if (!sprite_trace_active || !allocated || !have_published) {
		return NULL;
	}
	const trace_buffer_t *b = published;

	public_view.summary           = b->summary;
	public_view.gens              = b->gens;
	public_view.gen_count         = b->gen_count;
	public_view.mutations         = b->mutations;
	public_view.mutation_count    = b->mutation_count;
	public_view.mutations_dropped = b->mutation_dropped;
	public_view.lines             = b->lines;
	public_view.line_gen          = b->line_gen;
	public_view.line_flags        = b->line_flags;
	public_view.line_budget       = b->line_budget;
	return &public_view;
}
