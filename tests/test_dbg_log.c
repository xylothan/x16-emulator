// Checks for logpoint messages and their ring (src/dbg_log.c).

#include "dbg_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void
check(bool ok, const char *what)
{
	if (!ok) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

static uint8_t g_mem[0x10000];

static uint8_t
read_cpu(void *user, uint32_t addr, int bank)
{
	(void)user;
	(void)bank;
	return g_mem[addr & 0xFFFF];
}

static uint8_t
read_vram(void *user, uint32_t addr)
{
	(void)user;
	(void)addr;
	return 0x5A;
}

static uint32_t
read_reg(void *user, dbg_expr_reg_t reg)
{
	(void)user;
	return reg == DBG_EXPR_REG_A ? 0x05 : reg == DBG_EXPR_REG_X ? 0x1234 : 0;
}

static uint32_t g_sym = 0x0400;
static bool     g_sym_known = true;

static bool
symbol(void *user, const char *name, uint32_t *value, int *bank, char *err, size_t errn)
{
	(void)user;
	if (g_sym_known && !strcmp(name, "label")) {
		*value = g_sym;
		*bank  = DBG_EXPR_NO_BANK;
		return true;
	}
	if (!strcmp(name, "banked")) {
		*value = 0xA000;
		*bank  = 2;
		return true;
	}
	snprintf(err, errn, "unknown symbol %s", name);
	return false;
}

static const dbg_expr_env_t env = {NULL, read_cpu, read_vram, read_reg, symbol};

static void
expect(const char *message, const char *want)
{
	char err[160] = "", out[DBG_LOG_TEXT];
	dbg_log_msg_t *m = dbg_log_compile(message, &env, err, sizeof err);
	if (!m) {
		printf("FAIL: \"%s\" refused: %s\n", message, err);
		failures++;
		return;
	}
	dbg_log_format(m, &env, out, sizeof out);
	if (strcmp(out, want)) {
		printf("FAIL: \"%s\" gave \"%s\", wanted \"%s\"\n", message, out, want);
		failures++;
	}
	check(!strcmp(dbg_log_source(m), message), "the source text is kept verbatim");
	dbg_log_free(m);
}

static void
refused(const char *message, const char *reason_has)
{
	char err[160] = "";
	check(!dbg_log_check(message, err, sizeof err), message);
	check(strstr(err, reason_has) != NULL, reason_has);
	check(dbg_log_compile(message, &env, err, sizeof err) == NULL, message);
}

static void
test_messages(void)
{
	g_mem[0x0400] = 0x7F;
	expect("", "");
	expect("plain text", "plain text");
	expect("A={A}", "A=$05");
	expect("{A}{X}", "$05$1234");
	expect("x {X} y {A} z", "x $1234 y $05 z");
	expect("{byte[label]}", "$7F");
	expect("{banked}", "$02:A000");
	expect("{ A + 1 }", "$06");
	expect("{$12345}", "$012345");
	expect("{$12345678}", "$12345678");
	expect("{{A}}", "{A}");
	expect("{{{A}}}", "{$05}");
	expect("}}{{", "}{");
	expect("{vbyte[0]}", "$5A");

	refused("{", "unmatched {");
	refused("a { b", "unmatched {");
	refused("{a{b}", "unmatched {");
	refused("}", "unmatched }");
	refused("{A}}", "unmatched }");
	refused("{}", "empty");
	refused("{   }", "empty");

	char many[DBG_LOG_MAX_MESSAGE + 1] = "";
	for (int i = 0; i <= DBG_LOG_MAX_EXPRS; i++)
		strcat(many, "{A}");
	refused(many, "at most");

	char big[DBG_LOG_MAX_MESSAGE + 2];
	memset(big, 'a', sizeof big - 1);
	big[sizeof big - 1] = '\0';
	refused(big, "at most");
	big[sizeof big - 2] = '\0';
	expect(big, big);
}

static void
test_bad_expressions(void)
{
	char out[DBG_LOG_TEXT], err[160];

	dbg_log_msg_t *m = dbg_log_compile("a {nosuch} b {A}", &env, err, sizeof err);
	check(m != NULL, "a bad expression is not refused");
	dbg_log_format(m, &env, out, sizeof out);
	check(!strncmp(out, "a {nosuch: ", 11), "a bad expression prints with its text");
	check(strstr(out, "unknown symbol") != NULL, "and its reason");
	check(strstr(out, "} b $05") != NULL, "the rest of the line still prints");
	dbg_log_free(m);

	m = dbg_log_compile("{1/(A-5)}", &env, err, sizeof err);
	check(m != NULL, "an expression that fails only when run compiles");
	dbg_log_format(m, &env, out, sizeof out);
	check(!strncmp(out, "{1/(A-5): ", 10), "a failure while evaluating prints too");
	dbg_log_free(m);

	g_sym_known = false;
	m = dbg_log_compile("{label}", &env, err, sizeof err);
	dbg_log_format(m, &env, out, sizeof out);
	check(!strncmp(out, "{label: ", 8), "unknown until the symbol exists");
	g_sym_known = true;
	g_sym       = 0x0500;
	dbg_log_recompile(m, &env);
	dbg_log_format(m, &env, out, sizeof out);
	check(!strcmp(out, "$0500"), "recompiling picks the symbol up");
	g_sym = 0x0600;
	dbg_log_recompile(m, &env);
	dbg_log_format(m, &env, out, sizeof out);
	check(!strcmp(out, "$0600"), "and follows it when it moves");
	dbg_log_free(m);
}

static void
test_truncation(void)
{
	char text[DBG_LOG_MAX_MESSAGE + 1], out[DBG_LOG_TEXT], err[160];

	// {X} is 3 bytes and prints 5, so a full message overruns a line.
	memset(text, 'a', 207);
	text[207] = '\0';
	for (int i = 0; i < DBG_LOG_MAX_EXPRS; i++)
		strcat(text, "{X}");
	dbg_log_msg_t *m = dbg_log_compile(text, &env, err, sizeof err);
	check(m != NULL, "a long message compiles");
	dbg_log_format(m, &env, out, sizeof out);
	check(strlen(out) == DBG_LOG_TEXT - 1, "an overlong line fills the buffer");
	check(!strcmp(out + DBG_LOG_TEXT - 4, "..."), "and ends in ...");
	dbg_log_free(m);

	// A two-byte character straddling the cut is dropped whole.
	char small[9];
	m = dbg_log_compile("abc\xC3\xA9xyzzy", &env, err, sizeof err);
	dbg_log_format(m, &env, small, 7);
	check(!strcmp(small, "abc..."), "a cut never splits a UTF-8 character");
	dbg_log_format(m, &env, small, 8);
	check(!strcmp(small, "abc..."), "nor when one byte of it would fit");
	dbg_log_format(m, &env, small, 9);
	check(!strcmp(small, "abc\xC3\xA9..."), "a character that fits stays");
	dbg_log_free(m);
}

static dbg_log_ring_t g_ring;

static void
push(dbg_log_ring_t *r, uint8_t owner, int n)
{
	for (int i = 0; i < n; i++) {
		dbg_log_entry_t *e = dbg_log_ring_push(r, owner);
		snprintf(e->text, sizeof e->text, "%llu", (unsigned long long)e->seq);
	}
}

static void
test_ring(void)
{
	dbg_log_ring_t *r = &g_ring;
	memset(r, 0, sizeof *r);

	check(dbg_log_ring_oldest(r) == 0 && r->next == 0, "starts empty");
	check(dbg_log_ring_at(r, 0) == NULL, "an empty ring holds nothing");

	push(r, 1, DBG_LOG_RING);
	check(dbg_log_ring_oldest(r) == 0, "a full ring has lost nothing");
	check(!strcmp(dbg_log_ring_at(r, DBG_LOG_RING - 1)->text, "1023"), "the last line is there");

	push(r, 1, 5);
	check(dbg_log_ring_oldest(r) == 5, "overflow drops the oldest");
	check(dbg_log_ring_at(r, 4) == NULL, "a dropped line is gone");
	check(!strcmp(dbg_log_ring_at(r, 5)->text, "5"), "the next oldest stays");
	check(dbg_log_ring_at(r, r->next) == NULL, "past the end holds nothing");

	// A reader by sequence number changes nothing.
	const uint64_t before = r->next;
	(void)dbg_log_ring_at(r, 100);
	check(r->next == before && dbg_log_ring_oldest(r) == 5, "reading moves nothing");
}

static void
test_sink(void)
{
	dbg_log_ring_t *r = &g_ring;
	memset(r, 0, sizeof *r);
	uint64_t lost = 0;

	push(r, 2, 3);
	dbg_log_ring_sink(r, 1u << 2);
	check(dbg_log_ring_take(r, &lost) == NULL && lost == 0, "the sink starts at the end");

	push(r, 2, 1);
	push(r, 7, 1);
	push(r, 2, 1);
	const dbg_log_entry_t *e = dbg_log_ring_take(r, &lost);
	check(e && e->seq == 3, "takes its owner's line");
	e = dbg_log_ring_take(r, &lost);
	check(e && e->seq == 5, "skips another owner's");
	check(dbg_log_ring_take(r, &lost) == NULL && lost == 0, "then nothing, none lost");

	// Overwrites: only unread lines of its own owners count.
	memset(r, 0, sizeof *r);
	dbg_log_ring_sink(r, 1u << 2);
	push(r, 2, 4);
	for (int i = 0; i < 4; i++)
		check(dbg_log_ring_take(r, &lost) != NULL, "read before the overflow");
	push(r, 7, 2);                       // not this sink's
	push(r, 2, DBG_LOG_RING - 2);        // fills it; the 4 already read are overwritten
	check(dbg_log_ring_oldest(r) == 4, "the read lines were overwritten");
	lost = 0;
	e    = dbg_log_ring_take(r, &lost);
	check(lost == 0, "lines already read are not lost");
	check(e && e->seq == 6, "resumes after the skipped lines");

	push(r, 2, 10);                      // overwrites 2 of another owner, 1 read, 7 unread
	lost = 0;
	e    = dbg_log_ring_take(r, &lost);
	check(lost == 7, "counts only its own unread overwrites");
	check(e && e->seq == dbg_log_ring_oldest(r), "and carries on from the oldest");
	lost = 0;
	(void)dbg_log_ring_take(r, &lost);
	check(lost == 0, "a loss is reported once");
}

int
main(void)
{
	test_messages();
	test_bad_expressions();
	test_truncation();
	test_ring();
	test_sink();
	if (failures) {
		printf("\nFAILED (%d failures)\n", failures);
		return 1;
	}
	printf("\nPASSED (0 failures)\n");
	return 0;
}
