// Logpoint messages and their ring. See dbg_log.h.

#include "dbg_log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	uint16_t lit_start, lit_len;   // the literal text before it, in lits
	uint16_t src_start, src_len;   // the expression, in source
} log_span_t;

typedef struct {
	log_span_t span;
	bool       ok;
	char       err[64];
	dbg_expr_t expr;
} log_part_t;

struct dbg_log_msg {
	char        source[DBG_LOG_MAX_MESSAGE + 1];
	char        lits[DBG_LOG_MAX_MESSAGE + 1];   // literal text, escapes resolved
	uint16_t    tail_start, tail_len;            // literal text after the last part
	int         count;
	log_part_t *parts;
};

// Cut back to whole UTF-8 characters, so that a truncated line is still valid.
static size_t
utf8_floor(const char *s, size_t len)
{
	size_t i = len;
	while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80)
		i--;
	if (i == 0)
		return len;
	const unsigned char lead = (unsigned char)s[i - 1];
	const size_t        need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
	return len - (i - 1) >= need ? len : i - 1;
}

static bool
parse(const char *text, char *lits, log_span_t *spans, int *count, uint16_t *tail_start,
      uint16_t *tail_len, char *err, size_t errn)
{
	const size_t len = strlen(text);
	if (len > DBG_LOG_MAX_MESSAGE) {
		snprintf(err, errn, "a log message is at most %d bytes", DBG_LOG_MAX_MESSAGE);
		return false;
	}

	size_t   out = 0;
	uint16_t run = 0;
	int      n   = 0;
	for (size_t i = 0; i < len;) {
		const char c = text[i];
		if ((c == '{' || c == '}') && text[i + 1] == c) {
			lits[out++] = c;
			i += 2;
		} else if (c == '}') {
			snprintf(err, errn, "unmatched } in the log message: write }} for a literal brace");
			return false;
		} else if (c == '{') {
			size_t end = i + 1;
			while (end < len && text[end] != '}' && text[end] != '{')
				end++;
			if (end >= len || text[end] != '}') {
				snprintf(err, errn, "unmatched { in the log message: write {{ for a literal brace");
				return false;
			}
			bool blank = true;
			for (size_t k = i + 1; k < end; k++)
				blank = blank && isspace((unsigned char)text[k]);
			if (blank) {
				snprintf(err, errn, "empty {} in the log message");
				return false;
			}
			if (n >= DBG_LOG_MAX_EXPRS) {
				snprintf(err, errn, "a log message has at most %d {expressions}", DBG_LOG_MAX_EXPRS);
				return false;
			}
			spans[n].lit_start = run;
			spans[n].lit_len   = (uint16_t)(out - run);
			spans[n].src_start = (uint16_t)(i + 1);
			spans[n].src_len   = (uint16_t)(end - i - 1);
			n++;
			run = (uint16_t)out;
			i   = end + 1;
		} else {
			lits[out++] = c;
			i++;
		}
	}
	lits[out]   = '\0';
	*count      = n;
	*tail_start = run;
	*tail_len   = (uint16_t)(out - run);
	return true;
}

bool
dbg_log_check(const char *text, char *err, size_t errn)
{
	char       lits[DBG_LOG_MAX_MESSAGE + 1];
	log_span_t spans[DBG_LOG_MAX_EXPRS];
	int        count;
	uint16_t   tail_start, tail_len;
	return parse(text, lits, spans, &count, &tail_start, &tail_len, err, errn);
}

dbg_log_msg_t *
dbg_log_compile(const char *text, const dbg_expr_env_t *env, char *err, size_t errn)
{
	log_span_t spans[DBG_LOG_MAX_EXPRS];
	dbg_log_msg_t *m = (dbg_log_msg_t *)calloc(1, sizeof *m);
	if (!m) {
		snprintf(err, errn, "out of memory");
		return NULL;
	}
	if (!parse(text, m->lits, spans, &m->count, &m->tail_start, &m->tail_len, err, errn)) {
		free(m);
		return NULL;
	}
	if (m->count) {
		m->parts = (log_part_t *)calloc((size_t)m->count, sizeof *m->parts);
		if (!m->parts) {
			free(m);
			snprintf(err, errn, "out of memory");
			return NULL;
		}
	}
	memcpy(m->source, text, strlen(text) + 1);
	for (int i = 0; i < m->count; i++)
		m->parts[i].span = spans[i];
	dbg_log_recompile(m, env);
	return m;
}

void
dbg_log_recompile(dbg_log_msg_t *m, const dbg_expr_env_t *env)
{
	char text[DBG_LOG_MAX_MESSAGE + 1];
	for (int i = 0; i < m->count; i++) {
		log_part_t *p = &m->parts[i];
		memcpy(text, m->source + p->span.src_start, p->span.src_len);
		text[p->span.src_len] = '\0';
		p->ok = dbg_expr_compile(text, env, &p->expr, p->err, sizeof p->err);
		if (!p->ok)
			p->err[utf8_floor(p->err, strlen(p->err))] = '\0';
	}
}

void
dbg_log_free(dbg_log_msg_t *m)
{
	if (m) {
		free(m->parts);
		free(m);
	}
}

const char *
dbg_log_source(const dbg_log_msg_t *m)
{
	return m->source;
}

typedef struct {
	char  *out;
	size_t cap;    // room for text, not counting the NUL
	size_t len;
	bool   cut;
} log_out_t;

static void
put(log_out_t *o, const char *s, size_t n)
{
	if (n > o->cap - o->len) {
		n = o->cap - o->len;
		o->cut = true;
	}
	memcpy(o->out + o->len, s, n);
	o->len += n;
}

static void
put_value(log_out_t *o, const dbg_expr_value_t *v)
{
	char          buf[24];
	const unsigned u = v->value;
	int            n;
	if (v->bank >= 0)
		n = snprintf(buf, sizeof buf, "$%02X:%04X", (unsigned)v->bank, u);
	else
		n = snprintf(buf, sizeof buf, "$%0*X",
		             u <= 0xFF ? 2 : u <= 0xFFFF ? 4 : u <= 0xFFFFFF ? 6 : 8, u);
	put(o, buf, (size_t)n);
}

void
dbg_log_format(const dbg_log_msg_t *m, const dbg_expr_env_t *env, char *out, size_t n)
{
	if (n == 0)
		return;
	log_out_t o = {out, n - 1, 0, false};
	for (int i = 0; i < m->count; i++) {
		const log_part_t *p = &m->parts[i];
		put(&o, m->lits + p->span.lit_start, p->span.lit_len);

		dbg_expr_value_t v = {0, DBG_EXPR_NO_BANK};
		char             why[64];
		const char      *reason = p->err;
		bool             ok     = p->ok;
		if (ok) {
			ok     = dbg_expr_eval(&p->expr, env, &v, why, sizeof why);
			reason = why;
		}
		if (ok) {
			put_value(&o, &v);
		} else {
			put(&o, "{", 1);
			put(&o, m->source + p->span.src_start, p->span.src_len);
			put(&o, ": ", 2);
			put(&o, reason, strlen(reason));
			put(&o, "}", 1);
		}
	}
	put(&o, m->lits + m->tail_start, m->tail_len);

	if (o.cut && o.cap >= 3) {
		const size_t keep = utf8_floor(out, o.cap - 3);
		memcpy(out + keep, "...", 3);
		o.len = keep + 3;
	}
	out[o.len] = '\0';
}

// ---- The ring -------------------------------------------------------------

dbg_log_entry_t *
dbg_log_ring_push(dbg_log_ring_t *r, uint8_t owner)
{
	dbg_log_entry_t *e = &r->entries[r->next % DBG_LOG_RING];
	if (r->next >= DBG_LOG_RING && ((r->sink_mask >> e->owner) & 1) && e->seq >= r->sink_next)
		r->sink_lost++;
	e->seq     = r->next++;
	e->owner   = owner;
	e->text[0] = '\0';
	return e;
}

uint64_t
dbg_log_ring_oldest(const dbg_log_ring_t *r)
{
	return r->next > DBG_LOG_RING ? r->next - DBG_LOG_RING : 0;
}

const dbg_log_entry_t *
dbg_log_ring_at(const dbg_log_ring_t *r, uint64_t seq)
{
	if (seq >= r->next || seq < dbg_log_ring_oldest(r))
		return NULL;
	return &r->entries[seq % DBG_LOG_RING];
}

void
dbg_log_ring_sink(dbg_log_ring_t *r, uint16_t owner_mask)
{
	r->sink_mask = owner_mask;
	r->sink_next = r->next;
	r->sink_lost = 0;
}

const dbg_log_entry_t *
dbg_log_ring_take(dbg_log_ring_t *r, uint64_t *lost)
{
	*lost += r->sink_lost;
	r->sink_lost = 0;

	const uint64_t oldest = dbg_log_ring_oldest(r);
	if (r->sink_next < oldest)
		r->sink_next = oldest;
	while (r->sink_next < r->next) {
		const dbg_log_entry_t *e = &r->entries[r->sink_next++ % DBG_LOG_RING];
		if ((r->sink_mask >> e->owner) & 1)
			return e;
	}
	return NULL;
}
