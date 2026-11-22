// The I/O trace ring buffer: retention, ordering, gating and the CPU stamp.
// Keeps the latest events in order; sequence numbers survive resize and clear.

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

	// A resize discards the contents, but the sequence must not restart.
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
	// A requested capacity is clamped at both ends; the accessor reports what was allocated.
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

	// Default mask: everything except VERA's registers and open-bus reads.
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
	// A call before the ring is allocated must not crash; it allocates on demand.
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
	// An unnamed device would show as a blank column.
	for (int d = 0; d < IO_DEV_COUNT; d++) {
		const char *name = io_trace_device_name((io_device_t)d);
		check(name != NULL && name[0] != '\0' && strcmp(name, "?") != 0,
		      "every device has a display name");
	}
	check(strcmp(io_trace_device_name((io_device_t)IO_DEV_COUNT), "?") == 0,
	      "and an unknown one is named rather than left dangling");
}

static void
release_all_owners(void)
{
	for (int o = 0; o < IO_TRACE_OWNER_COUNT; o++) {
		io_trace_set_owner_mask((io_trace_owner_t)o, 0);
	}
}

static void
test_owner_masks(void)
{
	release_all_owners();
	check(!io_trace_enabled, "no owner, no capture");
	check_eq(io_trace_device_mask, 0, "and an empty effective mask");

	io_trace_set_owner_mask(IO_TRACE_OWNER_UI, IO_TRACE_BIT(IO_DEV_VIA1));
	io_trace_set_owner_mask(IO_TRACE_OWNER_MCP, IO_TRACE_BIT(IO_DEV_VERA));
	io_trace_set_owner_mask(IO_TRACE_OWNER_DAP, IO_TRACE_BIT(IO_DEV_VERA));
	check(io_trace_enabled, "any owner turns capture on");
	check_eq(io_trace_device_mask, IO_TRACE_BIT(IO_DEV_VIA1) | IO_TRACE_BIT(IO_DEV_VERA),
	         "the effective mask is the OR");

	io_trace_set_owner_mask(IO_TRACE_OWNER_MCP, 0);
	check(io_trace_wants(IO_DEV_VERA), "another owner's bit survives a release");
	check_eq(io_trace_owner_mask(IO_TRACE_OWNER_UI), IO_TRACE_BIT(IO_DEV_VIA1),
	         "and a client's release leaves the UI's");

	io_trace_set_owner_mask(IO_TRACE_OWNER_DAP, 0);
	check(!io_trace_wants(IO_DEV_VERA), "the last holder's release clears the bit");
	check(io_trace_wants(IO_DEV_VIA1), "the UI's stays");

	io_trace_set_owner_mask(IO_TRACE_OWNER_UI, 0);
	check(!io_trace_enabled, "the last release turns capture off");

	io_trace_set_owner_mask(IO_TRACE_OWNER_DAP, 0xffffffffu);
	check_eq(io_trace_owner_mask(IO_TRACE_OWNER_DAP), IO_TRACE_MASK_ALL, "bits past the devices are dropped");
	io_trace_set_owner_mask(IO_TRACE_OWNER_COUNT, IO_TRACE_BIT(IO_DEV_VIA1));
	check_eq(io_trace_owner_mask(IO_TRACE_OWNER_COUNT), 0, "an unknown owner holds nothing");
	check_eq(io_trace_device_mask, IO_TRACE_MASK_ALL, "and changes nothing");
	release_all_owners();

	check((IO_TRACE_MASK_DEFAULT & IO_TRACE_BIT(IO_DEV_VERA)) == 0, "the default leaves VERA out");
	check((IO_TRACE_MASK_DEFAULT & IO_TRACE_BIT(IO_DEV_OPENBUS)) == 0, "and open bus");
	check((IO_TRACE_MASK_DEFAULT & ~IO_TRACE_MASK_ALL) == 0, "and names only real devices");
}

static void
test_positions(void)
{
	check(io_trace_init(64), "a ring for the position checks");
	io_trace_clear();
	capture_everything();

	const uint64_t start = io_trace_next_seq();
	check(io_trace_oldest_seq() == start, "an empty ring's oldest is its next");
	check(io_trace_at_seq(start) == NULL, "and nothing is held there");

	for (int i = 0; i < 100; i++) {
		io_trace_access(IO_DEV_VIA1, 0x9f00, (uint8_t)i, false);
	}
	check(io_trace_next_seq() == start + 100, "next counts every event");
	check(io_trace_oldest_seq() == start + 36, "oldest is next minus what is held");
	check(io_trace_at_seq(start + 35) == NULL, "an overwritten event is gone");
	check(io_trace_at_seq(start + 100) == NULL, "and a future one is not there");
	const io_event_t *ev = io_trace_at_seq(start + 36);
	check(ev != NULL && ev->value == 36, "the oldest held is the right event");
	ev = io_trace_at_seq(start + 99);
	check(ev != NULL && ev->value == 99, "and so is the newest");

	io_trace_clear();
	check(io_trace_next_seq() == start + 100, "a clear keeps the position");
	check(io_trace_oldest_seq() == start + 100, "and holds nothing");
	io_trace_access(IO_DEV_VIA1, 0x9f00, 0xAB, false);
	ev = io_trace_at_seq(start + 100);
	check(ev != NULL && ev->value == 0xAB, "recording resumes at that position");

	check(io_trace_init(128), "the ring can be resized");
	check(io_trace_next_seq() == start + 101, "a resize keeps the position");
	check(io_trace_oldest_seq() == start + 101, "and holds nothing");
}

static void
test_device_ids(void)
{
	for (int d = 0; d < IO_DEV_COUNT; d++) {
		const char *id = io_trace_device_id((io_device_t)d);
		check(id != NULL && id[0] != '\0', "every device has an id");
		check(io_trace_device_from_id(id) == (io_device_t)d, "which maps back to it");
		for (const char *c = id; c != NULL && *c; c++) {
			check((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9'), "ids are lowercase");
		}
	}
	check(io_trace_device_id(IO_DEV_COUNT) == NULL, "an unknown device has no id");
	check(io_trace_device_from_id("nope") == IO_DEV_COUNT, "an unknown id has no device");
	check(io_trace_device_from_id(NULL) == IO_DEV_COUNT, "nor has NULL");
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
	test_owner_masks();
	test_positions();
	test_device_ids();

	io_trace_shutdown();
	return x16_test_summary("io_trace");
}
