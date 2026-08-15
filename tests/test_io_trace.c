// The I/O trace ring buffer: retention, ordering, gating and the CPU stamp.
//
// The ring is what the I/O panel shows, so the properties worth pinning are the
// ones a reader of that panel relies on without thinking about it: that the
// events are the most recent ones, in the order they happened; that a device
// whose capture bit is clear contributes nothing; and that sequence numbers
// keep increasing so "later in the list" always means "later in time", even
// across a resize or a clear.
//
// io_trace.c deliberately has no emulator dependencies, so this test links it
// alone -- no CPU, no memory, no SDL.

#include "support/harness.h"

#include "../src/io_trace.h"

#include <string.h>

// A stand-in for memory.c's provider, so the stamping path is exercised without
// linking the CPU.
static uint16_t g_pc     = 0;
static uint8_t  g_bank   = 0;
static uint32_t g_cycles = 0;

static void
fake_context(uint16_t *pc, uint8_t *pc_bank, uint32_t *cycles)
{
	*pc      = g_pc;
	*pc_bank = g_bank;
	*cycles  = g_cycles;
}

static void
capture_everything(void)
{
	io_trace_enabled     = true;
	io_trace_device_mask = 0xffffffffu;
}

static void
test_retains_the_most_recent(void)
{
	check(io_trace_init(64), "a ring can be allocated");
	io_trace_clear();
	capture_everything();

	for (int i = 0; i < 100; i++) {
		io_trace_access(IO_DEV_VIA1, (uint16_t)(0x9f00 + (i & 0x0f)), (uint8_t)i, false);
	}

	check_eq((uint32_t)io_trace_count(), 64, "the ring holds its capacity, not every event");
	check_eq(io_trace_dropped(), 36, "and reports what it had to overwrite");

	// Index 0 is the oldest RETAINED event, which after 100 pushes into 64
	// slots is the 37th (value 36).
	const io_event_t *oldest = io_trace_at(0);
	check(oldest != NULL, "the oldest retained event is reachable");
	if (oldest != NULL) {
		check_eq(oldest->value, 36, "and it is the oldest one that survived");
	}

	const io_event_t *newest = io_trace_at(io_trace_count() - 1);
	check(newest != NULL, "the newest event is reachable");
	if (newest != NULL) {
		check_eq(newest->value, 99, "and it is the one most recently recorded");
	}

	check(io_trace_at(-1) == NULL, "an index below the range is refused");
	check(io_trace_at(io_trace_count()) == NULL, "and so is one past the end");
}

static void
test_sequence_numbers_only_increase(void)
{
	check(io_trace_init(128), "a ring can be reallocated");
	io_trace_clear();
	capture_everything();

	io_trace_access(IO_DEV_VIA1, 0x9f00, 0x11, false);
	const io_event_t *first = io_trace_at(0);
	const uint32_t    seq   = (first != NULL) ? first->seq : 0;

	// A resize throws the contents away, because ordering against a new ring is
	// not meaningful -- but the sequence must not restart, or two unrelated
	// events would claim the same moment.
	check(io_trace_init(256), "the ring can be resized");
	check_eq((uint32_t)io_trace_count(), 0, "which discards what it held");
	check_eq((uint32_t)io_trace_capacity(), 256, "and adopts the new capacity");

	io_trace_access(IO_DEV_VIA1, 0x9f00, 0x22, false);
	const io_event_t *after = io_trace_at(0);
	check(after != NULL, "and keeps recording afterwards");
	if (after != NULL) {
		check(after->seq > seq, "with a sequence number that did not restart");
	}

	// Ordering within the ring must follow the sequence.
	io_trace_access(IO_DEV_VIA1, 0x9f00, 0x33, false);
	const io_event_t *a = io_trace_at(0);
	const io_event_t *b = io_trace_at(1);
	if (a != NULL && b != NULL) {
		check(b->seq > a->seq, "later entries carry later sequence numbers");
	}
}

static void
test_capacity_is_clamped(void)
{
	// A capacity the caller asks for is a request, not an instruction: a ring
	// of four events is useless for reading a trace, and an enormous one is a
	// way to exhaust memory from a settings field. Both ends are clamped, and
	// the accessor reports what was actually allocated so the UI can show the
	// real number rather than the one it asked for.
	check(io_trace_init(1), "an absurdly small ring is accepted");
	check(io_trace_capacity() >= 64, "but clamped up to something usable");

	check(io_trace_init(1 << 30), "an absurdly large ring is accepted");
	check(io_trace_capacity() <= 262144, "but clamped down to something affordable");
}

static void
test_capture_gating(void)
{
	check(io_trace_init(64), "a ring for the gating checks");
	io_trace_clear();

	// Master switch off: nothing is wanted, whatever the mask says.
	io_trace_enabled     = false;
	io_trace_device_mask = 0xffffffffu;
	check(!io_trace_wants(IO_DEV_VIA1), "a disabled trace wants nothing");
	check(!io_trace_wants(IO_DEV_SDCARD), "not even from an unmasked device");

	// Master on, one device masked out.
	io_trace_enabled     = true;
	io_trace_device_mask = IO_TRACE_BIT(IO_DEV_SDCARD);
	check(io_trace_wants(IO_DEV_SDCARD), "an unmasked device is wanted");
	check(!io_trace_wants(IO_DEV_VERA), "a masked-out device is not");

	// The default mask is the one the panel starts with: everything except
	// VERA's registers, which would otherwise be the only thing in the ring,
	// and open-bus reads, which say nothing.
	io_trace_device_mask = IO_TRACE_MASK_DEFAULT;
	check(!io_trace_wants(IO_DEV_VERA), "VERA is excluded by default");
	check(!io_trace_wants(IO_DEV_OPENBUS), "and so is open bus");
	check(io_trace_wants(IO_DEV_SPI), "but the SD data path is not");
	check(io_trace_wants(IO_DEV_VIA1), "nor VIA1");
	check(io_trace_wants(IO_DEV_SDCARD), "nor decoded SD events");
}

static void
test_records_what_it_was_given(void)
{
	check(io_trace_init(64), "a ring for the payload checks");
	io_trace_clear();
	capture_everything();
	io_trace_set_context_provider(fake_context);

	g_pc     = 0xc123;
	g_bank   = 0x02;
	g_cycles = 999;

	io_trace_access(IO_DEV_VIA1, 0x9f01, 0x5a, true);

	const io_event_t *ev = io_trace_at(0);
	check(ev != NULL, "an access is recorded");
	if (ev != NULL) {
		check_eq(ev->addr, 0x9f01, "with the address it was given");
		check_eq(ev->value, 0x5a, "and the value");
		check(ev->is_write, "and the direction");
		check(ev->has_addr, "and is marked as naming a register");
		check_eq(ev->device, IO_DEV_VIA1, "and the device");
		check_eq(ev->kind, IO_EVENT_ACCESS, "and is an access, not a decoded event");
		check_eq(ev->pc, 0xc123, "stamped with the PC from the provider");
		check_eq(ev->pc_bank, 0x02, "and the program bank");
		check_eq(ev->cycles, 999, "and the cycle count");
	}

	io_trace_set_context_provider(NULL);
}

static void
test_decoded_events(void)
{
	check(io_trace_init(64), "a ring for the decoded-event checks");
	io_trace_clear();
	capture_everything();

	io_trace_event(IO_DEV_SDCARD, "CMD%d lba=%u", 17, 74216u);

	const io_event_t *ev = io_trace_at(0);
	check(ev != NULL, "a decoded event is recorded");
	if (ev != NULL) {
		check_eq(ev->kind, IO_EVENT_DECODED, "marked as decoded");
		check(strcmp(ev->text, "CMD17 lba=74216") == 0, "with its text formatted");
		check(!ev->has_addr, "and naming no register");
	}

	io_trace_event_at(IO_DEV_SPI, 0x9f3e, true, "byte out $%02X", 0x40);
	const io_event_t *at = io_trace_at(1);
	check(at != NULL, "a register-attributed decoded event is recorded");
	if (at != NULL) {
		check(at->has_addr, "and does name a register");
		check_eq(at->addr, 0x9f3e, "which is the one given");
		check(at->is_write, "with its direction");
	}

	// Over-long text must be truncated rather than overrun the fixed buffer.
	io_trace_clear();
	io_trace_event(IO_DEV_IEEE,
	               "%s",
	               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	               "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
	const io_event_t *big = io_trace_at(0);
	check(big != NULL, "an over-long decoded event is still recorded");
	if (big != NULL) {
		check_eq((uint32_t)strlen(big->text), IO_TRACE_TEXT_MAX - 1,
		         "with its text truncated to the buffer");
	}
}

static void
test_clear_keeps_the_ring_usable(void)
{
	check(io_trace_init(64), "a ring for the clear checks");
	capture_everything();

	io_trace_access(IO_DEV_VIA1, 0x9f00, 0x01, false);
	io_trace_clear();
	check_eq((uint32_t)io_trace_count(), 0, "clearing empties the ring");
	check_eq(io_trace_dropped(), 0, "and resets the overwrite count");
	check(io_trace_at(0) == NULL, "leaving nothing to read");

	io_trace_access(IO_DEV_VIA1, 0x9f00, 0x02, false);
	check_eq((uint32_t)io_trace_count(), 1, "and recording resumes");
}

static void
test_survives_use_before_init(void)
{
	// A call site that fires before anything allocated the ring must not crash.
	// The module allocates on demand instead, so that merely enabling capture
	// works rather than silently recording nothing.
	io_trace_shutdown();
	check_eq((uint32_t)io_trace_capacity(), 0, "the ring starts unallocated");
	capture_everything();

	io_trace_access(IO_DEV_VIA1, 0x9f00, 0x77, false);
	check(io_trace_capacity() > 0, "recording allocates on first use");
	check_eq((uint32_t)io_trace_count(), 1, "and the event is kept");

	const io_event_t *ev = io_trace_at(0);
	if (ev != NULL) {
		check_eq(ev->value, 0x77, "with its value intact");
	}

	io_trace_shutdown();
	check(io_trace_at(0) == NULL, "and shutting down leaves nothing readable");
}

static void
test_device_names(void)
{
	// The panel prints these, so an unnamed device would show as a blank column
	// rather than anything a reader could act on.
	for (int d = 0; d < IO_DEV_COUNT; d++) {
		const char *name = io_trace_device_name((io_device_t)d);
		check(name != NULL && name[0] != '\0' && strcmp(name, "?") != 0,
		      "every device has a display name");
	}
	check(strcmp(io_trace_device_name((io_device_t)IO_DEV_COUNT), "?") == 0,
	      "and an unknown one is named rather than left dangling");
}

int
main(void)
{
	test_retains_the_most_recent();
	test_sequence_numbers_only_increase();
	test_capacity_is_clamped();
	test_capture_gating();
	test_records_what_it_was_given();
	test_decoded_events();
	test_clear_keeps_the_ring_usable();
	test_survives_use_before_init();
	test_device_names();

	io_trace_shutdown();
	return x16_test_summary("io_trace");
}
