/* Fake6502 CPU emulator core v1.1 *******************
 * (c)2011 Mike Chambers (miker00lz@gmail.com)       *
 *****************************************************
 * v1.1 - Small bugfix in BIT opcode, but it was the *
 *        difference between a few games in my NES   *
 *        emulator working and being broken!         *
 *        I went through the rest carefully again    *
 *        after fixing it just to make sure I didn't *
 *        have any other typos! (Dec. 17, 2011)      *
 *                                                   *
 * v1.0 - First release (Nov. 24, 2011)              *
 *****************************************************
 * LICENSE: This source code is released into the    *
 * public domain, but if you use it please do give   *
 * credit. I put a lot of effort into writing this!  *
 *                                                   *
 *****************************************************
 * Fake6502 is a MOS Technology 6502 CPU emulation   *
 * engine in C. It was written as part of a Nintendo *
 * Entertainment System emulator I've been writing.  *
 *                                                   *
 * If you do discover an error in timing accuracy,   *
 * or operation in general please e-mail me at the   *
 * address above so that I can fix it. Thank you!    *
 *                                                   *
 *****************************************************
 * Usage:                                            *
 *                                                   *
 * Fake6502 requires you to provide two external     *
 * functions:                                        *
 *                                                   *
 * uint8_t read6502(uint16_t address, uint8_t bank)  *
 * void write6502(uint16_t address, uint8_t bank,    *
 * uint8_t value)                                    *
 *                                                   *
 * You may optionally pass Fake6502 the pointer to a *
 * function which you want to be called after every  *
 * emulated instruction. This function should be a   *
 * void with no parameters expected to be passed to  *
 * it.                                               *
 *                                                   *
 * This can be very useful. For example, in a NES    *
 * emulator, you check the number of clock ticks     *
 * that have passed so you can know when to handle   *
 * APU events.                                       *
 *                                                   *
 * To pass Fake6502 this pointer, use the            *
 * hookexternal(void *funcptr) function provided.    *
 *                                                   *
 * To disable the hook later, pass NULL to it.       *
 *****************************************************
 * Useful functions in this emulator:                *
 *                                                   *
 * void reset6502(bool c816)                         *
 *   - Call this once before you begin execution.    *
 *                                                   *
 * void exec6502(uint32_t tickcount)                 *
 *   - Execute 6502 code up to the next specified    *
 *     count of clock ticks.                         *
 *                                                   *
 * void step6502()                                   *
 *   - Execute a single instrution.                  *
 *                                                   *
 * void irq6502()                                    *
 *   - Trigger a hardware IRQ in the 6502 core.      *
 *                                                   *
 * void nmi6502()                                    *
 *   - Trigger an NMI in the 6502 core.              *
 *                                                   *
 * void hookexternal(void *funcptr)                  *
 *   - Pass a pointer to a void function taking no   *
 *     parameters. This will cause Fake6502 to call  *
 *     that function once after each emulated        *
 *     instruction.                                  *
 *                                                   *
 *****************************************************
 * Useful variables in this emulator:                *
 *                                                   *
 * uint32_t clockticks6502                           *
 *   - A running total of the emulated cycle count.  *
 *                                                   *
 * uint32_t instructions                             *
 *   - A running total of the total emulated         *
 *     instruction count. This is not related to     *
 *     clock cycle timing.                           *
 *                                                   *
 *****************************************************/

#include "fake6502.h"
#include "registers.h"
#include "irq_ctx.h"

#include <stdio.h>
#include <stdint.h>

// getvalue16() below is currently unused but kept for completeness. GCC and
// Clang warn about unused statics under -Wall -Werror; MSVC has no equivalent
// attribute and rejects __attribute__ outright.
#ifdef _MSC_VER
#define MAYBE_UNUSED
#else
#define MAYBE_UNUSED __attribute__((unused))
#endif

// 6502 / 65C816 registers

struct regs regs;

//helper variables
uint32_t instructions = 0; //keep track of total instructions executed
uint32_t clockticks6502 = 0, clockgoal6502 = 0;
uint16_t opcode_addr, oldpc, reladdr, value;
uint32_t ea;
uint32_t result;
uint8_t opcode, oldstatus;

bool warn_rockwell = true;

uint8_t penaltyop, penaltyaddr;
uint8_t penaltym = 0;
uint8_t penaltye = 0;
uint8_t penaltyn = 0;
uint8_t penaltyx = 0;
uint8_t penaltyd = 0;
uint8_t waiting = 0;

// Set by acc() and cleared before each instruction, because the obvious test --
// comparing addrtable[opcode] against acc -- is not reliable. acc() and imp()
// are both empty, and MSVC folds identical functions onto one address, so the
// comparison is true for every implied opcode there and false under GCC.
static bool addressing_is_acc = false;

//externally supplied functions
extern uint8_t read6502(uint16_t address, uint8_t bank);
extern void write6502(uint16_t address, uint8_t bank, uint8_t value);
extern void stop6502(uint16_t address, uint8_t bank);
extern void vp6502();
extern uint8_t memory_get_ram_bank();
extern uint8_t memory_get_rom_bank();

static void (*addrtable_c02[256])();
static void (*addrtable_c816[256])();
static void (*optable_c02[256])();
static void (*optable_c816[256])();

static const uint32_t ticktable_c02[256];
static const uint32_t ticktable_c816[256];

static void (**addrtable)();
static void (**optable)();
static const uint32_t *ticktable;

#include "support.h"
#include "modes.h"

void rockwell_warning(const char *instruction) {
    uint8_t pc_bank;

    if (opcode_addr < 0xa000) {
        pc_bank = 0;
    } else if (opcode_addr < 0xc000) {
        pc_bank = memory_get_ram_bank();
    } else {
        pc_bank = memory_get_rom_bank();
    }

    printf("Warning: encountered Rockwell instruction %s at $%02x:%04x.\n", instruction, pc_bank, opcode_addr);
    printf("\tFuture Commander X16 hardware may ship with a 65C816 CPU,\n");
    printf("\twhich does not support these instructions.\n");
    printf("\tThis will be the only warning given for Rockwell\n");
    printf("\tinstructions until the emulator is relaunched.\n");
    printf("\tPass -rockwell to the command line to suppress this warning.\n\n");

    warn_rockwell = false;
}

static uint8_t wrappedBankByte(uint32_t addr) {
    // The direct page and the stack both live in bank zero, so the second byte
    // of a 16-bit access wraps inside it rather than stepping into bank one.
    if (addrtable[opcode] == zp || addrtable[opcode] == zpx ||
        addrtable[opcode] == zpy || addrtable[opcode] == sr) {
        return 0;
    }
    else {
        return bank_byte(addr+1);
    }
}

static uint16_t getvalue(bool use16Bit) {
    if (addressing_is_acc) {
        return use16Bit ? regs.c : (uint16_t)regs.a;
    } else if (use16Bit) {
        return ((uint16_t)read6502(ea, bank_byte(ea)) | ((uint16_t)read6502(ea+1, wrappedBankByte(ea)) << 8));
    }
    return read6502(ea, bank_byte(ea));
}

static uint16_t MAYBE_UNUSED getvalue16() {
    return((uint16_t)read6502(ea, bank_byte(ea)) | ((uint16_t)read6502(ea+1, wrappedBankByte(ea)) << 8));
}

static void putvalue(uint16_t saveval, bool use16Bit) {
    if (addressing_is_acc) {
        if (use16Bit) {
            regs.c = saveval;
        } else {
            regs.a = (uint8_t)(saveval & 0x00FF);
        }
    } else if (use16Bit) {
        write6502(ea, bank_byte(ea), (saveval & 0x00FF));
        write6502(ea + 1, wrappedBankByte(ea), saveval >> 8);
    } else {
        write6502(ea, bank_byte(ea), (saveval & 0x00FF));
    }
}

#include "instructions.h"
#include "65c02.h"
#include "tables.h"

void nmi6502() {
    interrupt6502(INT_NMI);
    waiting = 0;
}

void irq6502() {
    if (!(regs.status & FLAG_INTERRUPT)) {
        interrupt6502(INT_IRQ);
    }
    waiting = 0;
}

uint8_t callexternal = 0;
void (*loopexternal)();

void exec6502(uint32_t tickcount) {
	if (waiting) {
		clockticks6502 += tickcount;
		clockgoal6502 = clockticks6502;
		return;
    }

    opcode_addr = regs.pc;

    clockgoal6502 += tickcount;

    while (clockticks6502 < clockgoal6502) {
        opcode = read6502(regs.pc++, regs.k);

        cpu_pin_status_flags();

        penaltyop = 0;
        penaltyaddr = 0;
        penaltym = 0;
        penaltye = 0;
        penaltyn = 0;
        penaltyx = 0;
        penaltyd = 0;
        addressing_is_acc = false;

        (*addrtable[opcode])();
        (*optable[opcode])();
        cpu_pin_stack_page();
        clockticks6502 += ticktable[opcode];

        // Keep this in sync with step6502().
        if (penaltyop && penaltyaddr) clockticks6502++;
        if (memory_16bit()) clockticks6502 += penaltym;
        if (index_16bit()) clockticks6502 += penaltyx;
        if (penaltyn && !regs.e) clockticks6502++;
        if (penaltye && regs.e) clockticks6502++;
        if (penaltyd) clockticks6502++;

        instructions++;

        if (callexternal) (*loopexternal)();
    }
}

void step6502() {
	if (waiting) {
		++clockticks6502;
		clockgoal6502 = clockticks6502;
		return;
	}

    opcode_addr = regs.pc;

    opcode = read6502(regs.pc++, regs.k);

    cpu_pin_status_flags();

    penaltyop = 0;
    penaltyaddr = 0;
    penaltym = 0;
    penaltye = 0;
    penaltyn = 0;
    penaltyx = 0;
    // Otherwise this sticks: after one misaligned direct-page access every later
    // instruction costs an extra cycle, and reset6502() doesn't clear it either.
    penaltyd = 0;
        addressing_is_acc = false;

    (*addrtable[opcode])();
    (*optable[opcode])();
    cpu_pin_stack_page();
    clockticks6502 += ticktable[opcode];

    if (penaltyop && penaltyaddr) clockticks6502++;
    if (memory_16bit()) clockticks6502 += penaltym;
    if (index_16bit()) clockticks6502 += penaltyx;
    if (penaltyn && !regs.e) clockticks6502++;
    if (penaltye && regs.e) clockticks6502++;
    if (penaltyd) clockticks6502 ++;
    clockgoal6502 = clockticks6502;

    instructions++;

    if (callexternal) (*loopexternal)();
}

void hookexternal(void *funcptr) {
    if (funcptr != (void *)NULL) {
        loopexternal = funcptr;
        callexternal = 1;
    } else callexternal = 0;
}

//  Fixes from http://6502.org/tutorials/65c02opcodes.html
//
//  65C02 Cycle Count differences.
//        ADC/SBC work differently in decimal mode.
//        The wraparound fixes may not be required.
