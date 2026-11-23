// The expression engine's view of the running machine. See dbg_machine.h.

#include "dbg_machine.h"

#include "cpu/fake6502.h"
#include "dbg_info.h"
#include "glue.h"
#include "memory.h"
#include "video.h"

uint16_t
dbg_machine_dp_add(uint16_t offset)
{
	if (regs.e && (regs.dp & 0x00FF) == 0)
		return (uint16_t)((regs.dp & 0xFF00) | (uint8_t)((uint8_t)(regs.dp & 0xFF) + (offset & 0xFF)));
	return (uint16_t)(regs.dp + offset);
}

static uint8_t
read_cpu(void *user, uint32_t addr, int x16_bank)
{
	(void)user;
	return debug_read6502((uint16_t)(addr & 0xFFFF), (uint8_t)(addr >> 16),
	                      x16_bank >= 0 ? x16_bank : USE_CURRENT_X16_BANK);
}

static uint8_t
read_vram(void *user, uint32_t addr)
{
	(void)user;
	return video_space_read(addr);
}

// Each register at the width the variables view and setVariable use for it.
static uint32_t
read_reg(void *user, dbg_expr_reg_t reg)
{
	(void)user;
	const bool native = regs.is65c816 && !regs.e;
	if (reg >= DBG_EXPR_REG_R0 && reg <= DBG_EXPR_REG_R15) {
		const uint16_t a = dbg_machine_dp_add((uint16_t)(0x02 + 2 * (reg - DBG_EXPR_REG_R0)));
		return (uint32_t)debug_read6502(a, 0, USE_CURRENT_X16_BANK) |
		       ((uint32_t)debug_read6502((uint16_t)(a + 1), 0, USE_CURRENT_X16_BANK) << 8);
	}
	switch (reg) {
		case DBG_EXPR_REG_A:
			return native && !(regs.status & FLAG_MEMORY_WIDTH) ? regs.c : regs.a;
		case DBG_EXPR_REG_X:
			return native && !(regs.status & FLAG_INDEX_WIDTH) ? regs.x : (uint8_t)regs.x;
		case DBG_EXPR_REG_Y:
			return native && !(regs.status & FLAG_INDEX_WIDTH) ? regs.y : (uint8_t)regs.y;
		case DBG_EXPR_REG_SP:
			return native ? regs.sp : (uint8_t)regs.sp;
		case DBG_EXPR_REG_P:
			return regs.status;
		case DBG_EXPR_REG_PC:
			return (is_gen2 || regs.is65c816) ? ((uint32_t)regs.k << 16) | regs.pc : regs.pc;
		case DBG_EXPR_REG_RAMBANK:
			return memory_get_ram_bank();
		case DBG_EXPR_REG_ROMBANK:
			return memory_get_rom_bank();
		default:
			return 0;
	}
}

static bool
symbol(void *user, const char *name, uint32_t *value, int *bank, char *err, size_t errn)
{
	(void)user;
	dbg_resolved_t r;
	if (!dbg_info_resolve(name, &r, err, errn))
		return false;
	*value = r.addr;
	// A label file's bank applies from $A000 up. Below that, banks select nothing.
	*bank = r.addr >= 0xA000 && r.addr <= 0xFFFF ? r.bank : DBG_EXPR_NO_BANK;
	return true;
}

const dbg_expr_env_t *
dbg_machine_env(void)
{
	static const dbg_expr_env_t env = {NULL, read_cpu, read_vram, read_reg, symbol, false};
	return &env;
}
