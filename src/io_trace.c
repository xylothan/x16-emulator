// Commander X16 Emulator — I/O event trace ring buffer.
//
// See io_trace.h for what this is and why it has no emulator dependencies.

#include "io_trace.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IO_TRACE_CAPACITY_DEFAULT 4096
#define IO_TRACE_CAPACITY_MIN     64
#define IO_TRACE_CAPACITY_MAX     262144

bool     io_trace_enabled     = false;
uint32_t io_trace_device_mask = IO_TRACE_MASK_DEFAULT;

static io_event_t *s_ring  = NULL;
static int         s_cap   = 0;
static int         s_count = 0;
static int         s_head  = 0; // where the next event lands
static uint32_t    s_seq   = 0;
static uint32_t    s_total = 0;
static uint32_t    s_dropped = 0;

static io_trace_context_fn s_context = NULL;

void
io_trace_set_context_provider(io_trace_context_fn fn)
{
	s_context = fn;
}

bool
io_trace_init(int capacity)
{
	if (capacity < IO_TRACE_CAPACITY_MIN) {
		capacity = IO_TRACE_CAPACITY_MIN;
	} else if (capacity > IO_TRACE_CAPACITY_MAX) {
		capacity = IO_TRACE_CAPACITY_MAX;
	}

	io_event_t *ring = (io_event_t *)calloc((size_t)capacity, sizeof(io_event_t));
	if (ring == NULL) {
		return false;
	}

	free(s_ring);
	s_ring  = ring;
	s_cap   = capacity;
	s_count = 0;
	s_head  = 0;
	// s_seq, s_total and s_dropped deliberately survive: the sequence number is
	// the user's only stable handle on "when", and resetting it on a capacity
	// change would make two events look simultaneous.
	return true;
}

void
io_trace_shutdown(void)
{
	free(s_ring);
	s_ring  = NULL;
	s_cap   = 0;
	s_count = 0;
	s_head  = 0;
}

void
io_trace_clear(void)
{
	s_count = 0;
	s_head  = 0;
	s_dropped = 0;
}

int
io_trace_capacity(void)
{
	return s_cap;
}

int
io_trace_count(void)
{
	return s_count;
}

uint32_t
io_trace_total(void)
{
	return s_total;
}

uint32_t
io_trace_dropped(void)
{
	return s_dropped;
}

const io_event_t *
io_trace_at(int index)
{
	if (s_ring == NULL || index < 0 || index >= s_count) {
		return NULL;
	}
	const int oldest = (s_head - s_count + s_cap) % s_cap;
	return &s_ring[(oldest + index) % s_cap];
}

// Claims the next slot, stamped with the CPU context and a fresh sequence
// number. Returns NULL when the ring was never allocated, which is what keeps
// every call site safe before io_trace_init().
static io_event_t *
push(io_device_t dev, io_event_kind_t kind)
{
	if (s_ring == NULL) {
		// Allocate on first use so that simply enabling capture works, rather
		// than silently recording nothing until someone remembers to init.
		if (!io_trace_init(IO_TRACE_CAPACITY_DEFAULT)) {
			return NULL;
		}
	}

	io_event_t *ev = &s_ring[s_head];
	s_head         = (s_head + 1) % s_cap;
	if (s_count < s_cap) {
		s_count++;
	} else {
		s_dropped++;
	}

	memset(ev, 0, sizeof(*ev));
	ev->seq    = ++s_seq;
	ev->device = (uint8_t)dev;
	ev->kind   = (uint8_t)kind;
	s_total++;

	if (s_context != NULL) {
		s_context(&ev->pc, &ev->pc_bank, &ev->cycles);
	}
	return ev;
}

void
io_trace_access(io_device_t dev, uint16_t addr, uint8_t value, bool is_write)
{
	io_event_t *ev = push(dev, IO_EVENT_ACCESS);
	if (ev == NULL) {
		return;
	}
	ev->addr     = addr;
	ev->value    = value;
	ev->is_write = is_write;
	ev->has_addr = true;
}

static void
format_into(io_event_t *ev, const char *fmt, va_list ap)
{
	int n = vsnprintf(ev->text, sizeof(ev->text), fmt, ap);
	if (n < 0) {
		ev->text[0] = '\0';
	}
}

void
io_trace_event(io_device_t dev, const char *fmt, ...)
{
	io_event_t *ev = push(dev, IO_EVENT_DECODED);
	if (ev == NULL) {
		return;
	}
	va_list ap;
	va_start(ap, fmt);
	format_into(ev, fmt, ap);
	va_end(ap);
}

void
io_trace_event_at(io_device_t dev, uint16_t addr, bool is_write, const char *fmt, ...)
{
	io_event_t *ev = push(dev, IO_EVENT_DECODED);
	if (ev == NULL) {
		return;
	}
	ev->addr     = addr;
	ev->is_write = is_write;
	ev->has_addr = true;

	va_list ap;
	va_start(ap, fmt);
	format_into(ev, fmt, ap);
	va_end(ap);
}

const char *
io_trace_device_name(io_device_t dev)
{
	switch (dev) {
		case IO_DEV_VIA1: return "VIA1";
		case IO_DEV_VIA2: return "VIA2";
		case IO_DEV_VERA: return "VERA";
		case IO_DEV_SPI: return "SPI";
		case IO_DEV_YM: return "YM2151";
		case IO_DEV_EMU: return "Emu";
		case IO_DEV_MIDI: return "MIDI";
		case IO_DEV_OPENBUS: return "open bus";
		case IO_DEV_SDCARD: return "SD card";
		case IO_DEV_IEEE: return "Files";
		case IO_DEV_I2C: return "I2C";
		case IO_DEV_JOYSTICK: return "Joystick";
		case IO_DEV_SERIAL: return "Serial";
		default: return "?";
	}
}
