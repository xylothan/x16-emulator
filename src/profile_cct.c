// Commander X16 Emulator — calling-context tree. See profile_cct.h.

#include "profile_cct.h"

#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES 1024  // shadow stack; deeper than any 6502 stack can be
#define MAX_LEVELS 17    // main line plus 16 nested interrupts
#define MAX_QUEUE  4     // calls/returns from one instruction

enum { F_CALL, F_IRQ };
enum { Q_CALL, Q_RETURN };

typedef struct {
	uint64_t start;      // levels[level] when opened
	int32_t  node;       // node this frame made innermost
	int32_t  resume;     // innermost node to restore when it closes
	uint16_t sp_before;
	uint8_t  kind;
	uint8_t  level;
} frame_t;

typedef struct {
	uint32_t key;
	uint16_t sp;
	uint8_t  kind;
} queued_t;

static cct_node_t *nodes;
static uint32_t   *node_frame_acc;  // this frame's cycles of calls ended so far
static uint32_t   *node_frame_id;
static int         max_nodes, nnodes, normal_limit;
static int32_t    *table;           // (parent, key) -> node, open addressing
static uint32_t    table_mask;
static bool        overflowed;

static frame_t  frames[MAX_FRAMES];
static int      depth;
static int      cur;                // innermost node
static int      level;              // interrupt frames open (capped)
static uint64_t levels[MAX_LEVELS]; // cycles counted at each level
static uint32_t frame_id;

static queued_t queue[MAX_QUEUE];
static int      nqueued;

void (*cct_switch_hook)(int node) = NULL;

static void
announce(int before)
{
	if (cct_switch_hook && cur != before)
		cct_switch_hook(cur);
}

// ─── Nodes ──────────────────────────────────────────────────────────────────

static uint32_t
hash(int32_t parent, uint32_t key)
{
	return ((uint32_t)parent * 0x9E3779B1u) ^ (key * 0x85EBCA77u) ^ (key >> 13);
}

static int
new_node(int32_t parent, uint32_t key, uint32_t slot)
{
	const int n = nnodes++;
	memset(&nodes[n], 0, sizeof nodes[n]);
	nodes[n].key    = key;
	nodes[n].parent = parent;
	node_frame_acc[n] = 0;
	node_frame_id[n]  = 0;
	table[slot]       = n;
	return n;
}

static int
child(int32_t parent, uint32_t key)
{
	uint32_t h = hash(parent, key) & table_mask;
	for (;;) {
		const int32_t n = table[h];
		if (n < 0)
			break;
		if (nodes[n].parent == parent && nodes[n].key == key)
			return n;
		h = (h + 1) & table_mask;
	}
	if (nnodes < normal_limit)
		return new_node(parent, key, h);
	// Full: fold new paths into one [other] per parent, from the reserve.
	overflowed = true;
	if (key == CCT_KEY_OTHER)
		return nnodes < max_nodes ? new_node(parent, key, h) : parent;
	return child(parent, CCT_KEY_OTHER);
}

// ─── Frames ─────────────────────────────────────────────────────────────────

static void
flush_frame_acc(int n)
{
	if (node_frame_id[n] != frame_id) {
		if (node_frame_acc[n] > nodes[n].frame_max)
			nodes[n].frame_max = node_frame_acc[n];
		node_frame_acc[n] = 0;
		node_frame_id[n]  = frame_id;
	}
}

static void
pop(void)
{
	const frame_t *f   = &frames[--depth];
	cct_node_t    *n   = &nodes[f->node];
	const uint64_t d64 = levels[f->level] - f->start;
	const uint32_t d   = d64 > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)d64;
	n->calls++;
	if (d > n->call_max)
		n->call_max = d;
	flush_frame_acc(f->node);
	node_frame_acc[f->node] += d;
	if (f->kind == F_IRQ && level > 0)
		level--;
	cur = f->resume;
}

// Retire every frame the stack has risen past.
static void
retire(uint16_t sp)
{
	while (depth > 0 && frames[depth - 1].sp_before <= sp)
		pop();
}

static frame_t *
push(void)
{
	if (depth == MAX_FRAMES) {
		// Deeper than the 6502 stack allows: something is not returning.
		// Forget the outermost frame rather than the newest.
		memmove(&frames[0], &frames[1], sizeof frames[0] * (MAX_FRAMES - 1));
		depth--;
	}
	frame_t *f = &frames[depth++];
	memset(f, 0, sizeof *f);
	return f;
}

static void
apply_call(uint32_t key, uint16_t sp)
{
	retire(sp);
	const int n  = child(cur, key);
	frame_t  *f  = push();
	f->kind      = F_CALL;
	f->node      = n;
	f->resume    = cur;
	f->sp_before = sp;
	f->level     = (uint8_t)level;
	f->start     = levels[level];
	cur          = n;
}

static void
apply_return(uint16_t sp)
{
	// Below the innermost frame: nothing returned. RTS as a computed jump.
	if (depth == 0 || frames[depth - 1].sp_before > sp)
		return;
	retire(sp);
}

static void
apply_queue(void)
{
	const int before = cur;
	for (int i = 0; i < nqueued; i++) {
		const queued_t *q = &queue[i];
		if (q->kind == Q_CALL)
			apply_call(q->key, q->sp);
		else
			apply_return(q->sp);
	}
	nqueued = 0;
	announce(before);
}

static void
enqueue(uint8_t kind, uint32_t key, uint16_t sp)
{
	if (nqueued == MAX_QUEUE)
		apply_queue(); // cannot happen for real code; keep order regardless
	queue[nqueued].kind = kind;
	queue[nqueued].key  = key;
	queue[nqueued].sp   = sp;
	nqueued++;
}

// ─── API ────────────────────────────────────────────────────────────────────

bool
cct_init(int want)
{
	cct_free();
	if (want < 64)
		want = 64;
	uint32_t cap = 1;
	while (cap < (uint32_t)want * 2u)
		cap <<= 1;
	nodes          = malloc(sizeof *nodes * (size_t)want);
	node_frame_acc = malloc(sizeof *node_frame_acc * (size_t)want);
	node_frame_id  = malloc(sizeof *node_frame_id * (size_t)want);
	table          = malloc(sizeof *table * cap);
	if (!nodes || !node_frame_acc || !node_frame_id || !table) {
		cct_free();
		return false;
	}
	max_nodes    = want;
	normal_limit = want - (want / 16 > 16 ? want / 16 : 16);
	table_mask   = cap - 1;
	cct_reset();
	return true;
}

void
cct_free(void)
{
	free(nodes);
	free(node_frame_acc);
	free(node_frame_id);
	free(table);
	nodes          = NULL;
	node_frame_acc = NULL;
	node_frame_id  = NULL;
	table          = NULL;
	max_nodes = nnodes = normal_limit = 0;
	depth = cur = level = nqueued = 0;
}

void
cct_reset(void)
{
	if (!nodes)
		return;
	memset(table, 0xFF, sizeof *table * (table_mask + 1));
	nnodes     = 0;
	overflowed = false;
	// The two roots sit outside the hash: nothing looks them up by key.
	memset(&nodes[0], 0, sizeof nodes[0] * 2);
	nodes[CCT_NODE_MAIN].key    = CCT_KEY_MAIN;
	nodes[CCT_NODE_MAIN].parent = -1;
	nodes[CCT_NODE_IRQ].key     = CCT_KEY_IRQ;
	nodes[CCT_NODE_IRQ].parent  = -1;
	node_frame_acc[0] = node_frame_acc[1] = 0;
	node_frame_id[0] = node_frame_id[1] = 0;
	nnodes   = 2;
	depth    = 0;
	cur      = CCT_NODE_MAIN;
	level    = 0;
	nqueued  = 0;
	frame_id = 0;
	memset(levels, 0, sizeof levels);
}

void
cct_call(uint32_t target_key, uint16_t sp_before)
{
	if (nodes)
		enqueue(Q_CALL, target_key, sp_before);
}

void
cct_return(uint16_t sp_after)
{
	if (nodes)
		enqueue(Q_RETURN, 0, sp_after);
}

void
cct_irq_enter(uint32_t handler_key, uint16_t sp_before)
{
	if (!nodes)
		return;
	const int before = cur;
	retire(sp_before);
	const int n = child(CCT_NODE_IRQ, handler_key);
	frame_t  *f = push();
	if (level < MAX_LEVELS - 1)
		level++;
	f->kind      = F_IRQ;
	f->node      = n;
	f->resume    = cur;
	f->sp_before = sp_before;
	f->level     = (uint8_t)level;
	f->start     = levels[level];
	cur          = n;
	announce(before);
}

void
cct_irq_leave(uint16_t sp_after)
{
	if (!nodes)
		return;
	const int before = cur;
	apply_return(sp_after);
	announce(before);
}

void
cct_step(uint32_t clocks)
{
	if (!nodes)
		return;
	nodes[cur].self += clocks;
	levels[level] += clocks;
	if (nqueued)
		apply_queue();
}

void
cct_close_frame(void)
{
	frame_id++;
}

void
cct_finish(void)
{
	if (!nodes)
		return;
	apply_queue();
	while (depth > 0)
		pop();
	cur   = CCT_NODE_MAIN;
	level = 0;
	frame_id++;
	for (int i = 0; i < nnodes; i++) {
		flush_frame_acc(i);
		nodes[i].incl = nodes[i].self;
	}
	// A child is always created after its parent, so one backward pass sums
	// every subtree.
	for (int i = nnodes - 1; i >= 2; i--)
		nodes[nodes[i].parent].incl += nodes[i].incl;
}

int  cct_node_count(void) { return nnodes; }
int  cct_depth(void)      { return depth; }
int  cct_current(void)    { return cur; }
bool cct_overflowed(void) { return overflowed; }

bool
cct_node_get(int i, cct_node_t *out)
{
	if (i < 0 || i >= nnodes)
		return false;
	if (out)
		*out = nodes[i];
	return true;
}
