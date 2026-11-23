// Commander X16 Emulator — a shadow of the live JSRs. See c_frames.h.

#include "c_frames.h"

#include "cpu/irq_ctx.h"
#include "jmp_stub.h"

#include <stddef.h>
#include <string.h>

#ifdef _MSC_VER
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif

// Deepest call last, so slots strictly fall towards the end.
static c_frame_record_t  records[C_FRAMES_MAX];
static int               record_count;
static c_frames_config_t cfg;

// A return address at `slot` is on the stack only while SP is below it.
static inline void
retire(uint16_t sp)
{
	while (record_count > 0 && records[record_count - 1].slot <= sp)
		record_count--;
}

void
c_frames_configure(const c_frames_config_t *config)
{
	if (config)
		cfg = *config;
	else
		memset(&cfg, 0, sizeof cfg);
	record_count = 0;
}

void
c_frames_clear(void)
{
	record_count = 0;
}

// Out of line so that the common paths below save no registers.
static NOINLINE void
resolve_target(c_frame_record_t *r, uint16_t target)
{
	if ((int)target == cfg.callax)
		target = cfg.ax();
	r->target = jmp_stub_target(target, cfg.read);
}

static inline void
record_call(uint32_t pc, uint16_t sp, uint16_t ret)
{
	if (pc > 0xFFFF || record_count == C_FRAMES_MAX)
		return;

	const uint16_t    a      = cfg.c_sp_addr;
	const uint16_t    target = (uint16_t)pc;
	c_frame_record_t *r      = &records[record_count++];
	r->slot = (uint16_t)(sp - 1);
	r->ret  = ret;
	r->c_sp = (uint16_t)(cfg.low_ram[a] | (cfg.low_ram[a + 1] << 8));

	// Nearly every target is a function in low RAM that is not a stub.
	if (target < 0x9F00 && (int)target != cfg.callax) {
		const uint8_t op = cfg.low_ram[target];
		if (op != 0x4C && op != 0x6C) {
			r->target = target;
			return;
		}
	}
	resolve_target(r, target);
}

void
c_frames_flow(int kind, uint32_t pc, uint16_t sp, uint16_t ret)
{
	if (!cfg.low_ram)
		return;
	// With another direct page the program's zero page is not where c_sp was
	// read from, and a value recorded now would look right later.
	//
	// Up to three bytes move on any of these events. Near the bottom of the
	// page they wrap, and slots stop ordering by depth.
	if (!*cfg.e || *cfg.dp != 0 || (sp & 0xFF) < 3) {
		record_count = 0;
		return;
	}
	retire(sp);

	if (kind == CPU_FLOW_CALL)
		record_call(pc, sp, ret);
}

int
c_frames_live(uint16_t sp)
{
	retire(sp);
	return record_count;
}

bool
c_frames_at_slot(uint16_t sp, uint16_t slot, c_frame_record_t *out)
{
	retire(sp);
	for (int i = record_count - 1; i >= 0; i--) {
		if (records[i].slot == slot) {
			if (out)
				*out = records[i];
			return true;
		}
		if (records[i].slot > slot)
			break;
	}
	return false;
}

// 1 with one value, 0 with none, -1 when the hits disagree.
static int
pick_one(int count, c_frames_symbol_at_fn at, const char *want, uint32_t max, uint32_t *out)
{
	int found = 0;
	for (int i = 0; i < count; i++) {
		const char *name = NULL;
		uint32_t    addr = 0;
		if (!at(i, &name, &addr) || !name || strcmp(name, want) != 0 || addr > max)
			continue;
		if (found && addr != *out)
			return -1;
		*out  = addr;
		found = 1;
	}
	return found;
}

bool
c_frames_pick_symbols(int count, c_frames_symbol_at_fn at, uint16_t *c_sp_addr, int *callax)
{
	uint32_t sp_at = 0, call_at = 0;
	// The pointer's second byte has to be in the zero page too.
	int r = pick_one(count, at, "c_sp", 0xFE, &sp_at);
	if (r == 0)
		r = pick_one(count, at, "sp", 0xFE, &sp_at);

	*callax = pick_one(count, at, "callax", 0xFFFF, &call_at) == 1 ? (int)call_at : -1;
	if (r != 1)
		return false;
	*c_sp_addr = (uint16_t)sp_at;
	return true;
}
