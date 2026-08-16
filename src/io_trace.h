// Commander X16 Emulator — I/O event trace ring buffer.
//
// THE PROBLEM: the debugger can show you the CPU, memory, VERA and the audio
// sources, but I/O is invisible. When a program talks to the SD card, the
// joysticks, the SMC or the IEEE bus and gets the wrong answer back, there is
// nothing to look at. The state is spread across a dozen `.c` files as file
// statics, and the interesting part is not the state anyway -- it is the
// *sequence*: which command went out, what came back, in what order.
//
// THIS MODULE is the sequence half. It is a fixed-capacity ring of I/O events,
// fed from two places:
//
//   1. the $9F00-$9FFF dispatch in memory.c, which records a raw register
//      access (IO_EVENT_ACCESS), and
//   2. the devices themselves, which record decoded, human-readable events
//      (IO_EVENT_DECODED) such as "CMD17 READ_SINGLE_BLOCK lba=74216".
//
// WHY IT IS PER-DEVICE GATED: VERA's data ports are touched thousands of times
// per frame. Capturing them by default would push every other device out of the
// ring within a frame or two, so each device has its own capture bit and VERA
// starts off. `io_trace_wants()` is the inline predicate call sites use, so a
// device whose bit is clear costs one test-and-branch.
//
// WHY THIS FILE HAS NO EMULATOR DEPENDENCIES: memory.c is linked into
// tests/test_debugon_contract.c and tests/test_watchpoint_purity.c against
// recording fakes rather than the real devices. Anything memory.c calls has to
// link in that environment too, so this module includes nothing but libc and
// its own header. The CPU context stamped onto each event (PC, bank, cycle
// count) therefore arrives through a caller-installed provider rather than by
// reaching for `regs` and `clockticks6502` directly.
//
// THREADING: the emulator steps devices and renders the debugger on the same
// thread, so the ring needs no locking. The SDL audio callback thread runs
// independently but touches none of these devices. Do not call into this
// module from the audio callback.
#ifndef _IO_TRACE_H_
#define _IO_TRACE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The devices a trace event can be attributed to. The first group corresponds
// to address ranges in the I/O page; the second to devices that sit behind
// another device's registers and can only report themselves.
typedef enum {
	IO_DEV_VIA1 = 0,
	IO_DEV_VIA2,
	IO_DEV_VERA,
	IO_DEV_SPI, // VERA $9F3E/$9F3F -- the SD card data path, see below
	IO_DEV_YM,
	IO_DEV_EMU,
	IO_DEV_MIDI,
	IO_DEV_OPENBUS,
	IO_DEV_SDCARD, // decoded SD commands and responses
	IO_DEV_IEEE,   // host-filesystem file access, with real filenames
	IO_DEV_I2C,    // the I2C bus, and the SMC/RTC behind it
	IO_DEV_JOYSTICK,
	IO_DEV_SERIAL,
	IO_DEV_COUNT
} io_device_t;

// $9F3E/$9F3F are VERA registers, but they are also the *only* way the SD card
// is reached. They get their own device so that turning VERA capture off -- the
// default, because of the data ports -- does not also blind you to SD traffic.
// This is a deliberate departure from a strict address-range mapping.

typedef enum {
	IO_EVENT_ACCESS = 0, // a raw register read or write
	IO_EVENT_DECODED,    // a device-decoded event; `text` carries the meaning
} io_event_kind_t;

#define IO_TRACE_TEXT_MAX 72

typedef struct {
	uint32_t seq;     // monotonic, never reused; survives capacity changes
	uint32_t cycles;  // clockticks6502 when recorded (wraps, and $9FB8 resets it)
	uint16_t pc;      // CPU PC at the time of the access
	uint16_t addr;    // register address; 0 for decoded events with no register
	uint8_t  pc_bank; // 65C816 program bank
	uint8_t  value;
	uint8_t  device;  // io_device_t
	uint8_t  kind;    // io_event_kind_t
	bool     is_write;
	bool     has_addr; // false for decoded events that name no register
	char     text[IO_TRACE_TEXT_MAX];
} io_event_t;

// Master switch and the per-device capture bits. Both are read directly by the
// inline predicate below, so flipping either takes effect on the next access.
extern bool     io_trace_enabled;
extern uint32_t io_trace_device_mask;

#define IO_TRACE_BIT(dev) (1u << (unsigned)(dev))

// Everything except VERA's registers and open-bus reads. See the header comment
// for why VERA is excluded: it would otherwise be the only thing in the ring.
#define IO_TRACE_MASK_DEFAULT \
	(~(IO_TRACE_BIT(IO_DEV_VERA) | IO_TRACE_BIT(IO_DEV_OPENBUS)))

// The predicate every call site uses. Kept inline and side-effect-free so that
// a disabled trace costs a load and a branch on the I/O hot path.
static inline bool
io_trace_wants(io_device_t dev)
{
	return io_trace_enabled && (io_trace_device_mask & IO_TRACE_BIT(dev)) != 0;
}

// Supplies the CPU context stamped onto each event. Installed by memory.c;
// when absent, events are recorded with a zero PC and cycle count rather than
// not at all, so the module stays usable in unit tests.
typedef void (*io_trace_context_fn)(uint16_t *pc, uint8_t *pc_bank, uint32_t *cycles);
void io_trace_set_context_provider(io_trace_context_fn fn);

// Allocates the ring. Safe to call again to resize; existing events are
// discarded because their ordering relative to a new ring is not meaningful.
// Returns false if the allocation failed, in which case capture stays off.
bool io_trace_init(int capacity);
void io_trace_shutdown(void);
void io_trace_clear(void);

int      io_trace_capacity(void);
int      io_trace_count(void);
uint32_t io_trace_total(void);   // events ever recorded, including overwritten
uint32_t io_trace_dropped(void); // events overwritten by the ring wrapping

// Index 0 is the OLDEST retained event. Returns NULL when out of range.
const io_event_t *io_trace_at(int index);

// Record a raw register access. Call AFTER the device handler for reads, so the
// value is the one the machine actually saw; reads have side effects (VERA
// auto-increment, SPI byte consumption) and must never be repeated to observe
// them. Call BEFORE the handler for writes, so that any decoded event the
// device emits in response is ordered after the access that caused it.
void io_trace_access(io_device_t dev, uint16_t addr, uint8_t value, bool is_write);

// Record a decoded event. `fmt` is printf-style and the result is truncated to
// IO_TRACE_TEXT_MAX. Call sites should still guard with io_trace_wants() to
// avoid formatting work that will be thrown away.
void io_trace_event(io_device_t dev, const char *fmt, ...);

// As io_trace_event(), but attributed to a specific register.
void io_trace_event_at(io_device_t dev, uint16_t addr, bool is_write, const char *fmt, ...);

const char *io_trace_device_name(io_device_t dev);

#ifdef __cplusplus
}
#endif

#endif
