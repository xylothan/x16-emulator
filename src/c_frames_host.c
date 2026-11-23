// Commander X16 Emulator — c_frames, wired to the emulator. See c_frames_host.h.

#include "c_frames_host.h"

#include "c_frames.h"
#include "cpu/irq_ctx.h"
#include "dbg_info.h"
#include "glue.h"
#include "memory.h"

#include <stdio.h>

static bool     wanted;
static bool     listening;
static bool     refused_logged;
static int      logged_c_sp = -1;
static int      tracked_c_sp = -1;

static uint8_t
host_read(uint16_t a)
{
	return debug_read6502(a, 0, USE_CURRENT_X16_BANK);
}

static uint16_t
host_ax(void)
{
	return (uint16_t)(regs.a | (regs.xl << 8));
}

// Runs after anything changes what debug info is loaded. Records are dropped
// each time: they were matched against the functions that have just changed.
static void
sync(void)
{
	c_frames_config_t config = {0};
	tracked_c_sp = -1;
	const bool arm = wanted && dbg_info_cfunc_count() > 0 &&
	                 c_frames_pick_symbols(dbg_info_symbol_count(), dbg_info_symbol_at,
	                                       &config.c_sp_addr, &config.callax);
	if (!arm) {
		if (listening) {
			cpu_flow_unlisten(c_frames_flow);
			listening = false;
		}
		c_frames_configure(NULL);
		logged_c_sp = -1;
		return;
	}

	config.low_ram = RAM;
	config.e       = &regs.e;
	config.dp      = &regs.dp;
	config.read    = host_read;
	config.ax      = host_ax;
	c_frames_configure(&config);
	if (!listening)
		listening = cpu_flow_listen(c_frames_flow);
	if (!listening) {
		if (!refused_logged)
			fprintf(stderr, "c_frames: the CPU flow hook is full; C frames are not tracked\n");
		refused_logged = true;
		c_frames_configure(NULL);
		return;
	}
	tracked_c_sp = config.c_sp_addr;
	if (logged_c_sp != config.c_sp_addr) {
		fprintf(stderr, "c_frames: tracking C calls, C stack pointer at $%02X\n", config.c_sp_addr);
		logged_c_sp = config.c_sp_addr;
	}
}

void
c_frames_host_start(bool debugger)
{
	wanted = debugger;
	dbg_info_set_change_hook(sync);
	sync();
}

bool
c_frames_host_c_sp(uint16_t *addr)
{
	if (tracked_c_sp < 0)
		return false;
	*addr = (uint16_t)tracked_c_sp;
	return true;
}
