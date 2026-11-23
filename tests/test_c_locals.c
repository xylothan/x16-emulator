// Checks for the C variable resolver (src/c_locals.c).

#include "c_locals.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void
check(bool cond, const char *what)
{
	if (!cond) {
		failures++;
		printf("FAIL: %s\n", what);
	} else {
		printf("ok  : %s\n", what);
	}
}

#define ENTRY   0x0840
#define PUSHA   0x0900
#define PUSHAX  0x0910
#define PUSHEAX 0x0920
#define ENTER   0x0930
#define OTHER   0x0940
#define UNNAMED 0x0950

static uint8_t mem[0x10000];

static uint8_t
read_mem(uint16_t a)
{
	return mem[a];
}

static bool
is_named(uint16_t a)
{
	return a != UNNAMED;
}

static const c_locals_helpers_t k_helpers = {PUSHA, PUSHAX, PUSHEAX, ENTER};

static void
plant_jsr(uint16_t target)
{
	mem[ENTRY]     = 0x20;
	mem[ENTRY + 1] = (uint8_t)target;
	mem[ENTRY + 2] = (uint8_t)(target >> 8);
}

static int
push_of(uint16_t target, bool *variadic)
{
	plant_jsr(target);
	return c_locals_push_bytes(ENTRY, read_mem, &k_helpers, is_named, variadic);
}

typedef struct {
	const char *name;
	uint32_t    addr;
} fake_sym_t;

static const fake_sym_t *sym_table;

static bool
sym_at(int i, const char **name, uint32_t *addr)
{
	*name = sym_table[i].name;
	*addr = sym_table[i].addr;
	return true;
}

static dbg_cvar_t
var(const char *name, int offs)
{
	dbg_cvar_t v = {name, offs, false, false};
	return v;
}

static c_frame_view_t
known(int base, uint16_t c_sp)
{
	c_frame_view_t f = {C_FRAME_KNOWN, base, c_sp, false, false};
	return f;
}

#define MAIN_START 0x0A00
#define MAIN_START_LEN 23

static void
plant_main_start(void)
{
	static const uint8_t code[MAIN_START_LEN] = {
		0xAD, 0x00, 0x03, 0xAE, 0x01, 0x03, 0x20, PUSHAX & 0xFF, PUSHAX >> 8,
		0xAD, 0x02, 0x03, 0xAE, 0x03, 0x03, 0x20, PUSHAX & 0xFF, PUSHAX >> 8,
		0xA0, 0x04, 0x4C, ENTRY & 0xFF, ENTRY >> 8,
	};
	memcpy(&mem[MAIN_START], code, sizeof code);
}

int
main(void)
{
	bool variadic = false;

	// ── Startup jump to main ────────────────────────────────────────────────
	memset(mem, 0xEA, sizeof mem);
	plant_main_start();
	check(c_locals_is_main_start(MAIN_START, ENTRY, read_mem, &k_helpers), "callmain is recognised");
	check(!c_locals_is_main_start(MAIN_START, ENTRY + 1, read_mem, &k_helpers),
	      "callmain jumping elsewhere is refused");
	{
		bool all_refused = true;
		for (int i = 0; i < MAIN_START_LEN; i++) {
			mem[MAIN_START + i] ^= 0x01;
			if (c_locals_is_main_start(MAIN_START, ENTRY, read_mem, &k_helpers))
				all_refused = false;
			mem[MAIN_START + i] ^= 0x01;
		}
		check(all_refused, "any changed byte is refused");
		const c_locals_helpers_t absent = {PUSHA, -1, PUSHEAX, ENTER};
		const c_locals_helpers_t odd    = {PUSHA, -2, PUSHEAX, ENTER};
		check(!c_locals_is_main_start(MAIN_START, ENTRY, read_mem, &absent), "no pushax symbol");
		check(!c_locals_is_main_start(MAIN_START, ENTRY, read_mem, &odd), "disagreeing pushax symbols");
		check(!c_locals_is_main_start(0x9F00 - MAIN_START_LEN + 1, ENTRY, read_mem, &k_helpers),
		      "code past plain RAM is refused");
	}

	// ── Prologue ────────────────────────────────────────────────────────────
	memset(mem, 0xEA, sizeof mem);
	check(c_locals_push_bytes(ENTRY, read_mem, &k_helpers, is_named, &variadic) == 0 && !variadic,
	      "a first instruction that is no JSR pushes nothing");
	check(push_of(PUSHA, &variadic) == 1 && !variadic, "jsr pusha");
	check(push_of(PUSHAX, &variadic) == 2 && !variadic, "jsr pushax");
	check(push_of(PUSHEAX, &variadic) == 4 && !variadic, "jsr pusheax");
	check(push_of(ENTER, &variadic) == 1 && variadic, "jsr enter is variadic");
	check(push_of(OTHER, &variadic) == 0 && !variadic, "a JSR to another named symbol");
	check(push_of(UNNAMED, &variadic) == -1, "a JSR to an unnamed address is refused");

	{
		const c_locals_helpers_t odd = {PUSHA, -2, PUSHEAX, ENTER};
		plant_jsr(OTHER);
		check(c_locals_push_bytes(ENTRY, read_mem, &odd, is_named, &variadic) == -1,
		      "helpers that disagree refuse a JSR");
		mem[ENTRY] = 0xA9;
		check(c_locals_push_bytes(ENTRY, read_mem, &odd, is_named, &variadic) == 0,
		      "but not another first instruction");
	}

	// ── Finding the helpers ─────────────────────────────────────────────────
	{
		static const fake_sym_t t[] = {{"pushaxx", 1}, {"pusha", 0x900}, {"pushax", 0x910},
		                               {"pushax", 0x910}, {"enter", 0x930}, {"enter", 0x931}};
		c_locals_helpers_t h;
		sym_table = t;
		c_locals_pick_helpers(6, sym_at, &h);
		check(h.pusha == 0x900, "exact name");
		check(h.pushax == 0x910, "a repeat at one address is one helper");
		check(h.pusheax == -1, "absent");
		check(h.enter == -2, "two addresses disagree");
	}

	// ── Entry pending ───────────────────────────────────────────────────────
	check(c_locals_entry_pending(true, ENTRY, ENTRY, 2), "frame 0 at the entry");
	check(!c_locals_entry_pending(true, ENTRY + 3, ENTRY, 2), "frame 0 after the JSR returned");
	check(c_locals_entry_pending(false, ENTRY + 3, ENTRY, 2), "a caller inside the entry JSR");
	check(!c_locals_entry_pending(false, ENTRY + 9, ENTRY, 2), "a caller further on");
	check(!c_locals_entry_pending(true, ENTRY, ENTRY, 0), "nothing to store");

	// ── Matching a record ───────────────────────────────────────────────────
	{
		const c_frame_record_t r = {0x01F0, 0x0812, 0x9E00, ENTRY};
		check(c_locals_record_matches(&r, 0x01F0, 0x0813, ENTRY, 0x12, 0x08), "all four agree");
		check(!c_locals_record_matches(&r, 0x01EE, 0x0813, ENTRY, 0x12, 0x08), "another slot");
		check(!c_locals_record_matches(&r, 0x01F0, 0x0814, ENTRY, 0x12, 0x08), "another return");
		check(!c_locals_record_matches(&r, 0x01F0, 0x0813, ENTRY, 0x13, 0x08), "stack low byte changed");
		check(!c_locals_record_matches(&r, 0x01F0, 0x0813, ENTRY, 0x12, 0x09), "stack high byte changed");
		check(!c_locals_record_matches(&r, 0x01F0, 0x0813, ENTRY + 1, 0x12, 0x08), "another function");
	}

	// ── Each status ─────────────────────────────────────────────────────────
	{
		const dbg_cvar_t v[] = {var("p2", 2), var("p1", 0), var("l1", -2), var("l2", -6)};
		c_frame_view_t f = known(0x9D00, 0x9CF0);
		c_local_t r;

		c_locals_var(&f, v, 4, 0, &r);
		check(r.status == C_LOCAL_OK && r.is_param && r.address == 0x9D02 && r.size == 0,
		      "highest offset: address, size unknown");
		c_locals_var(&f, v, 4, 1, &r);
		check(r.status == C_LOCAL_OK && r.is_param && r.address == 0x9D00 && r.size == 2, "offset 0");
		c_locals_var(&f, v, 4, 2, &r);
		check(r.status == C_LOCAL_OK && !r.is_param && r.address == 0x9CFE && r.size == 2, "local");
		c_locals_var(&f, v, 4, 3, &r);
		check(r.status == C_LOCAL_OK && r.address == 0x9CFA && r.size == 4, "size from the gap");

		f = known(0x9D00, 0x9CFE);
		c_locals_var(&f, v, 4, 3, &r);
		check(r.status == C_LOCAL_UNALLOCATED && r.has_address && r.address == 0x9CFA,
		      "below c_sp: address only");
		c_locals_var(&f, v, 4, 2, &r);
		check(r.status == C_LOCAL_OK, "at c_sp is allocated");

		f = known(0x9D00, 0x9CF0);
		f.entry_pending = true;
		c_locals_var(&f, v, 4, 1, &r);
		check(r.status == C_LOCAL_NOT_STORED && !r.has_address, "last parameter during the prologue");
		c_locals_var(&f, v, 4, 0, &r);
		check(r.status == C_LOCAL_OK, "an earlier parameter is stored");

		for (int s = C_FRAME_NOT_SEEN; s <= C_FRAME_PROLOGUE; s++) {
			f = known(0x9D00, 0x9CF0);
			f.state = (c_frame_state_t)s;
			c_locals_var(&f, v, 4, 2, &r);
			check(r.status == C_LOCAL_UNKNOWN_FRAME && !r.has_address, c_frame_state_name(f.state));
			check(c_frame_state_reason(f.state)[0] != '\0', "with a reason");
		}
	}

	// ── Variadic ────────────────────────────────────────────────────────────
	{
		const dbg_cvar_t v[] = {var("fixed", 2), var("last", 0), var("l1", -2), var("l2", -4)};
		c_frame_view_t f = known(0x9D00, 0x9CF0);
		f.variadic = true;
		c_local_t r;
		c_locals_var(&f, v, 4, 0, &r);
		check(r.status == C_LOCAL_REFUSED_VARIADIC && !r.has_address, "fixed parameter");
		c_locals_var(&f, v, 4, 1, &r);
		check(r.status == C_LOCAL_REFUSED_VARIADIC, "last fixed parameter");
		c_locals_var(&f, v, 4, 2, &r);
		check(r.status == C_LOCAL_OK && r.address == 0x9CFE && r.size == 0,
		      "a local beside a fixed parameter has no size");
		c_locals_var(&f, v, 4, 3, &r);
		check(r.status == C_LOCAL_OK && r.size == 2, "a local between locals has one");
	}

	// ── Register variables ──────────────────────────────────────────────────
	{
		dbg_cvar_t v[] = {var("a", 0), var("r", 4), var("b", -2)};
		v[1].is_register = true;
		c_frame_view_t f = known(0x9D00, 0x9CF0);
		c_local_t r;
		c_locals_var(&f, v, 3, 1, &r);
		check(r.status == C_LOCAL_REFUSED_REGISTER && !r.is_param && !r.has_address, "sc=reg");
		f.state = C_FRAME_NOT_SEEN;
		c_locals_var(&f, v, 3, 1, &r);
		check(r.status == C_LOCAL_REFUSED_REGISTER, "whatever the frame");
		f = known(0x9D00, 0x9CF0);
		c_locals_var(&f, v, 3, 2, &r);
		check(r.status == C_LOCAL_OK && r.size == 0, "no sizes in a function with one");
	}

	// ── Location ────────────────────────────────────────────────────────────
	{
		const dbg_cvar_t v[] = {var("hi", 4), var("x", 0), var("neg", -0x7000)};
		c_local_t r;
		c_frame_view_t f = known(0x9EFE, 0x0100);
		c_locals_var(&f, v, 3, 1, &r);
		check(r.status == C_LOCAL_REFUSED_LOCATION && !r.has_address, "a run crossing $9F00");
		f = known(0x9EFC, 0x0100);
		c_locals_var(&f, v, 3, 1, &r);
		check(r.status == C_LOCAL_OK && r.address == 0x9EFC && r.size == 4, "a run ending at $9EFF");
		c_locals_var(&f, v, 3, 0, &r);
		check(r.status == C_LOCAL_REFUSED_LOCATION, "size unknown, first byte at $9F00");
		f = known(0x1000, 0x0002);
		c_locals_var(&f, v, 3, 2, &r);
		check(r.status == C_LOCAL_REFUSED_LOCATION, "below zero");
		f = known(0x7001, 0x0000);
		c_locals_var(&f, v, 3, 2, &r);
		check(r.status == C_LOCAL_REFUSED_LOCATION, "a bank register");
		f = known(0x7002, 0x0000);
		c_locals_var(&f, v, 3, 2, &r);
		check(r.status == C_LOCAL_OK && r.address == 0x0002, "the first plain byte");
		f = known(-1, 0x0000);
		c_locals_var(&f, v, 3, 1, &r);
		check(r.status == C_LOCAL_REFUSED_LOCATION, "a base below zero");
	}

	// ── Names in expressions ────────────────────────────────────────────────
	{
		const dbg_cvar_t v[] = {var("a", 0), var("a0", 2), var("count", -2)};
		check(!strcmp(c_locals_expr_name("a", v, 3), "a"), "a bare name");
		check(!c_locals_expr_name("A", v, 3), "case-sensitive");
		check(!strcmp(c_locals_expr_name("byte[count]+1", v, 3), "count"), "inside an expression");
		check(!c_locals_expr_name("$a0", v, 3), "$hex");
		check(!c_locals_expr_name("0xa0", v, 3), "0xhex");
		check(!c_locals_expr_name("10a", v, 3), "a number");
		check(!c_locals_expr_name("counter", v, 3), "a longer name");
		check(!c_locals_expr_name("_a", v, 3), "another name");
		check(!strcmp(c_locals_expr_name("$10+a0", v, 3), "a0"), "after a number");
		check(!c_locals_expr_name("", v, 3), "empty");
	}

	// ── Names ───────────────────────────────────────────────────────────────
	check(!strcmp(c_local_status_name(C_LOCAL_OK), "ok"), "ok");
	check(!strcmp(c_local_status_name(C_LOCAL_UNALLOCATED), "unallocated"), "unallocated");
	check(!strcmp(c_local_status_name(C_LOCAL_NOT_STORED), "not-stored"), "not-stored");
	check(!strcmp(c_local_status_name(C_LOCAL_UNKNOWN_FRAME), "unknown-frame"), "unknown-frame");
	check(!strcmp(c_local_status_name(C_LOCAL_REFUSED_VARIADIC), "refused-variadic"), "refused-variadic");
	check(!strcmp(c_local_status_name(C_LOCAL_REFUSED_REGISTER), "refused-register"), "refused-register");
	check(!strcmp(c_local_status_name(C_LOCAL_REFUSED_LOCATION), "refused-location"), "refused-location");

	if (failures) {
		printf("%d failure(s)\n", failures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
