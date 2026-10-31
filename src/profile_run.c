// Commander X16 Emulator — a saved profile run. See profile_run.h.

// fseeko/ftello are POSIX, hidden by -std=c11 without this; 64-bit offsets so
// a run over 2 GB still works on 32-bit Linux.
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#endif

#include "profile_run.h"
#include "bytes_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RUN_VERSION 1
static const char MAGIC[8] = {'X', '1', '6', 'P', 'R', 'O', 'F', 0};

#define TAG(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define T_META TAG('M', 'E', 'T', 'A')
#define T_ROWS TAG('R', 'O', 'W', 'S')
#define T_NODE TAG('N', 'O', 'D', 'E')
#define T_KEYS TAG('K', 'E', 'Y', 'S')
#define T_WFAL TAG('W', 'F', 'A', 'L')
#define T_WORK TAG('W', 'O', 'R', 'K') // each vsync's CPU work
#define T_END  TAG('E', 'N', 'D', ' ')

#ifdef _WIN32
#define FTELL64 _ftelli64
#define FSEEK64 _fseeki64
#else
#define FTELL64 ftello
#define FSEEK64 fseeko
#endif

// ─── Writing ────────────────────────────────────────────────────────────────

// A section's tag and a placeholder length; section_end() fills the length in.
static int64_t
section_begin(FILE *f, uint32_t tag)
{
	bio_w32(f, tag);
	const int64_t at = FTELL64(f);
	bio_w64(f, 0);
	return at;
}

static void
section_end(FILE *f, int64_t at)
{
	const int64_t end = FTELL64(f);
	FSEEK64(f, at, SEEK_SET);
	bio_w64(f, (uint64_t)(end - at - 8));
	FSEEK64(f, end, SEEK_SET);
}

static int
cmp_key(const void *a, const void *b)
{
	const prof_run_key_t *x = a, *y = b;
	return (x->key > y->key) - (x->key < y->key);
}

int
prof_run_write(const char *path, const prof_run_meta_t *m, const prof_report_t *r, const prof_run_key_t *keys,
               int nkeys, bool with_waterfall, const uint32_t *work, uint32_t nwork, char *msg, size_t msg_size)
{
	if (!path || !m || !r) {
		snprintf(msg, msg_size, "Nothing to save.");
		return -1;
	}
	FILE *f = fopen(path, "wb");
	if (!f) {
		snprintf(msg, msg_size, "Could not write %s.", path);
		return -1;
	}
	fwrite(MAGIC, 1, sizeof MAGIC, f);
	bio_w32(f, RUN_VERSION);
	bio_w32(f, 0);

	// Metadata as text, so it reads in any editor and grows without a version.
	int64_t s = section_begin(f, T_META);
	fprintf(f, "captured=%s\nrevision=%s\nprogram=%s\ndbg=%s\nend=%s\n", m->captured, m->revision, m->program,
	        m->dbg, m->end_reason);
	fprintf(f, "mhz=%.6f\nelapsed=%.6f\nframes=%u\nrecorders=%u\ntree_consistent=%d\n", m->mhz, m->elapsed_s,
	        m->frames, m->recorders, m->tree_consistent ? 1 : 0);
	fprintf(f, "budget=%d %u %llu %llu %u %u %u\n", m->b_have ? 1 : 0, m->b_frames,
	        (unsigned long long)m->b_total, (unsigned long long)m->b_idle, m->b_work_max, m->b_overruns,
	        m->b_budget);
	// Separate lines rather than more fields on budget=, which older readers
	// parse with a fixed count.
	fprintf(f, "target_fps=%.4f\nvsync_cycles=%u\n", (double)m->b_target_fps, m->b_vsync_cycles);
	section_end(f, s);

	if (work && nwork) {
		s = section_begin(f, T_WORK);
		bio_w32(f, nwork);
		for (uint32_t i = 0; i < nwork; i++)
			bio_w32(f, work[i]);
		section_end(f, s);
	}

	s = section_begin(f, T_ROWS);
	bio_w64(f, r->main_cycles);
	bio_w64(f, r->irq_cycles);
	bio_w64(f, r->instructions);
	bio_w32(f, (uint32_t)r->named_rows);
	bio_w32(f, (uint32_t)r->ambiguous_rows);
	bio_w32(f, (uint32_t)r->count);
	for (int i = 0; i < r->count; i++) {
		const prof_row_t *w = &r->rows[i];
		bio_wstr(f, w->name);
		bio_w8(f, w->region);
		bio_w8(f, w->bank);
		bio_w16(f, w->addr);
		bio_w8(f, (uint8_t)((w->named ? 1 : 0) | (w->ambiguous ? 2 : 0)));
		bio_w64(f, w->main_cycles);
		bio_w64(f, w->irq_cycles);
		bio_w64(f, w->instructions);
		bio_w64(f, w->incl_cycles);
		bio_w32(f, w->calls);
		bio_w32(f, w->call_max);
		bio_w32(f, w->frame_max);
	}
	section_end(f, s);

	s = section_begin(f, T_NODE);
	bio_w32(f, (uint32_t)(r->nodes ? r->node_count : 0));
	for (int i = 0; r->nodes && i < r->node_count; i++) {
		const prof_node_t *n = &r->nodes[i];
		bio_w32(f, (uint32_t)n->parent);
		bio_w32(f, (uint32_t)n->row);
		bio_w32(f, n->key);
		bio_w64(f, n->self);
		bio_w64(f, n->incl);
		bio_w32(f, n->calls);
		bio_w32(f, n->call_max);
		bio_w32(f, n->frame_max);
	}
	section_end(f, s);

	s = section_begin(f, T_KEYS);
	bio_w32(f, (uint32_t)(keys ? nkeys : 0));
	for (int i = 0; keys && i < nkeys; i++) {
		bio_w32(f, keys[i].key);
		bio_w32(f, (uint32_t)keys[i].row);
		bio_wstr(f, keys[i].source ? keys[i].source : "");
	}
	section_end(f, s);

	if (with_waterfall) {
		s = section_begin(f, T_WFAL);
		wf_write(f);
		section_end(f, s);
	}

	s = section_begin(f, T_END);
	section_end(f, s);
	const bool ok = !ferror(f);
	if (fclose(f) != 0 || !ok) {
		remove(path);
		snprintf(msg, msg_size, "Could not write all of %s (disk full?).", path);
		return -1;
	}
	snprintf(msg, msg_size, "Saved %s.", path);
	return 0;
}

// ─── Reading ────────────────────────────────────────────────────────────────

static void
meta_line(prof_run_meta_t *m, const char *k, const char *v)
{
#define STR(field, key)                                         \
	if (!strcmp(k, key)) {                                      \
		snprintf(m->field, sizeof m->field, "%s", v);           \
		return;                                                 \
	}
	STR(captured, "captured")
	STR(revision, "revision")
	STR(program, "program")
	STR(dbg, "dbg")
	STR(end_reason, "end")
#undef STR
	if (!strcmp(k, "mhz"))
		m->mhz = atof(v);
	else if (!strcmp(k, "elapsed"))
		m->elapsed_s = atof(v);
	else if (!strcmp(k, "frames"))
		m->frames = (uint32_t)strtoul(v, NULL, 10);
	else if (!strcmp(k, "recorders"))
		m->recorders = (uint32_t)strtoul(v, NULL, 10);
	else if (!strcmp(k, "tree_consistent"))
		m->tree_consistent = atoi(v) != 0;
	else if (!strcmp(k, "target_fps"))
		m->b_target_fps = (float)atof(v);
	else if (!strcmp(k, "vsync_cycles"))
		m->b_vsync_cycles = (uint32_t)strtoul(v, NULL, 10);
	else if (!strcmp(k, "budget")) {
		int                have = 0;
		unsigned long long tot = 0, idle = 0;
		if (sscanf(v, "%d %u %llu %llu %u %u %u", &have, &m->b_frames, &tot, &idle, &m->b_work_max,
		           &m->b_overruns, &m->b_budget) == 7) {
			m->b_have  = have != 0;
			m->b_total = tot;
			m->b_idle  = idle;
		}
	}
}

static bool
read_meta(const uint8_t *p, size_t n, prof_run_meta_t *m)
{
	size_t i = 0;
	while (i < n) {
		size_t j = i;
		while (j < n && p[j] != '\n')
			j++;
		char line[4096];
		size_t len = j - i < sizeof line - 1 ? j - i : sizeof line - 1;
		memcpy(line, p + i, len);
		line[len] = 0;
		char *eq = strchr(line, '=');
		if (eq) {
			*eq = 0;
			meta_line(m, line, eq + 1);
		}
		i = j + 1;
	}
	return true;
}

static bool
read_rows(const uint8_t *p, size_t n, prof_report_t *r)
{
	bio_reader_t rd = {p, p + n, false};
	r->main_cycles    = bio_r64(&rd);
	r->irq_cycles     = bio_r64(&rd);
	r->instructions   = bio_r64(&rd);
	r->named_rows     = (int)bio_r32(&rd);
	r->ambiguous_rows = (int)bio_r32(&rd);
	const uint32_t count = bio_r32(&rd);
	// A row is at least 51 bytes; a count the section cannot hold is corrupt.
	if (!bio_room_for(&rd, count, 51) || count > 0x7FFFFFFF)
		return false;
	r->rows = calloc(count ? count : 1, sizeof *r->rows);
	if (!r->rows)
		return false;
	r->count = (int)count;
	for (uint32_t i = 0; i < count; i++) {
		prof_row_t *w = &r->rows[i];
		bio_rstr(&rd, w->name, sizeof w->name);
		w->region       = bio_r8(&rd);
		w->bank         = bio_r8(&rd);
		w->addr         = bio_r16(&rd);
		const uint8_t fl = bio_r8(&rd);
		w->named        = (fl & 1) != 0;
		w->ambiguous    = (fl & 2) != 0;
		w->main_cycles  = bio_r64(&rd);
		w->irq_cycles   = bio_r64(&rd);
		w->instructions = bio_r64(&rd);
		w->incl_cycles  = bio_r64(&rd);
		w->calls        = bio_r32(&rd);
		w->call_max     = bio_r32(&rd);
		w->frame_max    = bio_r32(&rd);
	}
	return !rd.bad;
}

static bool
read_nodes(const uint8_t *p, size_t n, prof_report_t *r)
{
	bio_reader_t   rd    = {p, p + n, false};
	const uint32_t count = bio_r32(&rd);
	if (!bio_room_for(&rd, count, 40) || count > 0x7FFFFFFF)
		return false;
	if (!count)
		return !rd.bad;
	r->nodes = calloc(count, sizeof *r->nodes);
	if (!r->nodes)
		return false;
	r->node_count = (int)count;
	for (uint32_t i = 0; i < count; i++) {
		prof_node_t *d = &r->nodes[i];
		d->parent      = (int32_t)bio_r32(&rd);
		d->row         = (int32_t)bio_r32(&rd);
		d->key         = bio_r32(&rd);
		d->self        = bio_r64(&rd);
		d->incl        = bio_r64(&rd);
		d->calls       = bio_r32(&rd);
		d->call_max    = bio_r32(&rd);
		d->frame_max   = bio_r32(&rd);
		// A parent must come before its child, and a row must exist.
		if (d->parent >= (int32_t)i || d->parent < -1 || d->row >= r->count || d->row < -1)
			return false;
	}
	return !rd.bad;
}

static bool
read_keys(const uint8_t *p, size_t n, prof_run_t *out)
{
	bio_reader_t   rd    = {p, p + n, false};
	const uint32_t count = bio_r32(&rd);
	if (!bio_room_for(&rd, count, 10) || count > 0x7FFFFFFF)
		return false;
	out->keys = calloc(count ? count : 1, sizeof *out->keys);
	if (!out->keys)
		return false;
	for (uint32_t i = 0; i < count; i++) {
		prof_run_key_t *k = &out->keys[i];
		out->nkeys        = (int)i + 1;
		k->key            = bio_r32(&rd);
		k->row            = (int32_t)bio_r32(&rd);
		char src[1024];
		bio_rstr(&rd, src, sizeof src);
		k->source = malloc(strlen(src) + 1);
		if (!k->source)
			return false;
		strcpy(k->source, src);
		if (k->row >= out->report.count || k->row < -1)
			k->row = -1;
	}
	qsort(out->keys, (size_t)out->nkeys, sizeof *out->keys, cmp_key);
	return !rd.bad;
}

static bool
read_work(const uint8_t *p, size_t n, prof_run_t *out)
{
	bio_reader_t   rd    = {p, p + n, false};
	const uint32_t count = bio_r32(&rd);
	if (!bio_room_for(&rd, count, 4) || count > 0x7FFFFFFF)
		return false;
	out->work = malloc(count ? sizeof *out->work * count : 1);
	if (!out->work)
		return false;
	for (uint32_t i = 0; i < count; i++)
		out->work[i] = bio_r32(&rd);
	out->nwork = count;
	return !rd.bad;
}

void
prof_run_free(prof_run_t *run)
{
	if (!run)
		return;
	prof_report_free(&run->report);
	for (int i = 0; run->keys && i < run->nkeys; i++)
		free(run->keys[i].source);
	free(run->keys);
	wf_staged_free(run->wf);
	free(run->work);
	memset(run, 0, sizeof *run);
}

int
prof_run_read(const char *path, prof_run_t *out, char *msg, size_t msg_size)
{
	memset(out, 0, sizeof *out);
	FILE *f = fopen(path, "rb");
	if (!f) {
		snprintf(msg, msg_size, "Could not open %s.", path);
		return -1;
	}
	FSEEK64(f, 0, SEEK_END);
	const int64_t size = FTELL64(f);
	FSEEK64(f, 0, SEEK_SET);

	char     magic[8];
	uint8_t  hdr[8];
	bool     ok = fread(magic, 1, 8, f) == 8 && memcmp(magic, MAGIC, 8) == 0 && fread(hdr, 1, 8, f) == 8;
	uint32_t version = ok ? (uint32_t)(hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | ((uint32_t)hdr[3] << 24)) : 0;
	if (!ok) {
		fclose(f);
		snprintf(msg, msg_size, "%s is not a saved profile run.", path);
		return -1;
	}
	if (version != RUN_VERSION) {
		fclose(f);
		snprintf(msg, msg_size, "%s was saved by a newer emulator (format %u).", path, version);
		return -1;
	}

	bool seen_rows = false, seen_end = false, bad = false;
	while (!bad && !seen_end) {
		uint8_t th[12];
		if (fread(th, 1, 12, f) != 12) {
			bad = true;
			break;
		}
		bio_reader_t   hr  = {th, th + 12, false};
		const uint32_t tag = bio_r32(&hr);
		const uint64_t len = bio_r64(&hr);
		const int64_t  at  = FTELL64(f);
		if (at < 0 || len > (uint64_t)(size - at)) {
			bad = true; // claims more than the file holds
			break;
		}
		if (tag == T_END) {
			seen_end = true;
			break;
		}
		if (tag != T_META && tag != T_ROWS && tag != T_NODE && tag != T_KEYS && tag != T_WFAL &&
		    tag != T_WORK) {
			FSEEK64(f, (int64_t)len, SEEK_CUR); // from a newer writer: not ours to read
			continue;
		}
		uint8_t *buf = malloc(len ? (size_t)len : 1);
		if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
			free(buf);
			bad = true;
			break;
		}
		switch (tag) {
			case T_META: bad = !read_meta(buf, (size_t)len, &out->meta); break;
			case T_ROWS:
				bad       = seen_rows || !read_rows(buf, (size_t)len, &out->report);
				seen_rows = true;
				break;
			case T_NODE: bad = !seen_rows || !read_nodes(buf, (size_t)len, &out->report); break;
			case T_KEYS: bad = !seen_rows || !read_keys(buf, (size_t)len, out); break;
			case T_WFAL:
				out->wf = wf_read(buf, (size_t)len);
				bad     = out->wf == NULL;
				break;
			case T_WORK: bad = out->work != NULL || !read_work(buf, (size_t)len, out); break;
		}
		free(buf);
	}
	fclose(f);
	if (bad || !seen_end || !seen_rows) {
		prof_run_free(out);
		snprintf(msg, msg_size, "%s is damaged or cut short; nothing was loaded.", path);
		return -1;
	}
	snprintf(msg, msg_size, "Loaded %s.", path);
	return 0;
}

const prof_run_key_t *
prof_run_find_key(const prof_run_key_t *keys, int n, uint32_t key)
{
	int lo = 0, hi = n - 1;
	while (lo <= hi) {
		const int mid = (lo + hi) / 2;
		if (keys[mid].key == key)
			return &keys[mid];
		if (keys[mid].key < key)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return NULL;
}
