/*

						Extracted from original single fake6502.c file

*/


//flag modifier macros
#define setcarry() regs.status |= FLAG_CARRY
#define clearcarry() regs.status &= (~FLAG_CARRY)
#define setzero() regs.status |= FLAG_ZERO
#define clearzero() regs.status &= (~FLAG_ZERO)
#define setinterrupt() regs.status |= FLAG_INTERRUPT
#define clearinterrupt() regs.status &= (~FLAG_INTERRUPT)
#define setdecimal() regs.status |= FLAG_DECIMAL
#define cleardecimal() regs.status &= (~FLAG_DECIMAL)
#define setoverflow() regs.status |= FLAG_OVERFLOW
#define clearoverflow() regs.status &= (~FLAG_OVERFLOW)
#define setsign() regs.status |= FLAG_SIGN
#define clearsign() regs.status &= (~FLAG_SIGN)


//flag calculation macros
#define zerocalc(n, use16Bit) {\
    if (use16Bit) \
        if ((n) & 0xFFFF) clearzero();\
            else setzero();\
    else if ((n) & 0x00FF) clearzero();\
        else setzero();\
}

#define signcalc(n, use16Bit) {\
    if (use16Bit) \
        if ((n) & 0x8000) setsign();\
            else clearsign();\
    else if ((n) & 0x0080) setsign();\
        else clearsign();\
}

#define carrycalc(n, use16Bit) {\
    if (use16Bit) \
        if ((n) & 0x10000) setcarry();\
            else clearcarry();\
    else if ((n) & 0x0100) setcarry();\
        else clearcarry();\
}

#define overflowcalc8(n, m, o) { /* n = result, m = accumulator, o = memory */ \
    if (((n) ^ (m)) & ((n) ^ (o)) & 0x80) setoverflow();\
        else clearoverflow();\
}

#define overflowcalc16(n, m, o) { /* n = result, m = accumulator, o = memory */ \
    if (((n) ^ (m)) & ((n) ^ (o)) & 0x8000) setoverflow();\
        else clearoverflow();\
}

#define index_16bit() (regs.is65c816 && !(regs.status & FLAG_INDEX_WIDTH))
#define memory_16bit() (regs.is65c816 && !(regs.status & FLAG_MEMORY_WIDTH))
#define acc_for_mode() (memory_16bit() ? regs.c : ((uint16_t) regs.a))

// Bits 4 and 5 of P mean different things on the two CPUs, so pin them before
// each instruction according to which one is running.
//
// On the 65C816 they are the index and memory width flags, forced to 1 in
// emulation mode.
//
// On the 65C02 bit 5 is unused and reads as 1, while bit 4 is the break flag,
// which has no register existence -- it only appears in a copy of P pushed to
// the stack. No instruction alters it, so it must be preserved. Forcing it
// here corrupts it: ProcessorTests has 22,900 of 25,400 cases expecting it
// clear.
static inline void
cpu_pin_status_flags(void)
{
    if (regs.is65c816) {
        if (regs.e) {
            regs.status |= FLAG_INDEX_WIDTH | FLAG_MEMORY_WIDTH;
        }
    } else {
        regs.status |= FLAG_CONSTANT;
    }
}

// PLP and RTI load P from the stack. On the 65C02 bits 4 and 5 are not
// loadable -- there is no register bit behind either -- so they keep their
// previous values. On the 65C816 both are real width flags and load normally.
static inline uint8_t
cpu_merge_pulled_status(uint8_t pulled)
{
    const uint8_t keep = FLAG_INDEX_WIDTH | FLAG_CONSTANT;
    if (regs.is65c816) {
        return pulled;
    }
    return (uint8_t) ((pulled & (uint8_t) ~keep) | (regs.status & keep));
}

// A 16-bit operand is one more byte through the bus, and each byte costs a
// cycle: the datasheet's "M = 0 or X = 0, 16 bit operation, add 1 cycle". A
// read-modify-write pays twice, once in each direction.
//
// Accumulator addressing touches no memory, so ASL A costs the same at either
// width and must not be charged.
#define pay_for_wide_memory(n) \
    (penaltym = addressing_is_acc ? 0 : (uint8_t)(n))


#define saveaccum(n) (memory_16bit() ? (regs.c = (n)) : (regs.a = (uint8_t)((n) & 0x00FF)))

#define mask_long_addr(addr) ((addr) & 0xFFFFFF)
#define as_bank_byte(b) ((uint32_t) (((uint8_t) (b)) << 16))
#define bank_byte(b) ((uint8_t) ((b) >> 16))

//a few general functions used by various other functions

uint16_t add_wrap_at_page_boundary(uint16_t value, uint8_t add) {
    if (regs.e) {
        return (value & 0xFF00) | ((uint16_t) (((uint8_t) (value & 0x00FF)) + add) & 0x00FF);
    } else {
        return value + add;
    }
}

uint16_t subtract_wrap_at_page_boundary(uint16_t value, uint8_t subtract) {
    if (regs.e) {
        return (value & 0xFF00) | ((uint16_t) (((uint8_t) (value & 0x00FF)) - subtract) & 0x00FF);
    } else {
        return value - subtract;
    }
}

void increment_wrap_at_page_boundary(uint16_t *value) {
    if (regs.e) {
        *value = (*value & 0xFF00) | ((uint16_t) (((uint8_t) (*value & 0x00FF)) + 1) & 0x00FF);
    } else {
        (*value)++;
    }
}

void decrement_wrap_at_page_boundary(uint16_t *value) {
    if (regs.e) {
        *value = (*value & 0xFF00) | ((uint16_t) (((uint8_t) (*value & 0x00FF)) - 1) & 0x00FF);
    } else {
        (*value)--;
    }
}

uint16_t direct_page_add(uint16_t offset) {
    if (regs.e && (regs.dp & 0x00FF) == 0) {
        return (regs.dp & 0xFF00) | ((uint16_t) ((uint8_t) (regs.dp & 0x00FF)) + (offset & 0xFF));
    } else {
        return regs.dp + offset;
    }
}

#define incsp() increment_wrap_at_page_boundary(&regs.sp)
#define decsp() decrement_wrap_at_page_boundary(&regs.sp)

void push16(uint16_t pushval) {
    write6502(regs.sp, 0, (pushval >> 8) & 0xFF);
    decsp();
    write6502(regs.sp, 0, pushval & 0xFF);
    decsp();
}

void push8(uint8_t pushval) {
    write6502(regs.sp, 0, pushval);
    decsp();
}

// One bus access per statement: C leaves the order of a | b unspecified.
static inline uint16_t read_pair(uint16_t lo, uint16_t hi, uint8_t bank) {
    const uint8_t l = read6502(lo, bank);
    const uint8_t h = read6502(hi, bank);
    return (uint16_t)(l | (h << 8));
}

static inline uint32_t read_long(uint16_t lo, uint16_t mid, uint16_t hi, uint8_t bank) {
    const uint8_t l = read6502(lo, bank);
    const uint8_t m = read6502(mid, bank);
    const uint8_t h = read6502(hi, bank);
    return (uint32_t)l | ((uint32_t)m << 8) | ((uint32_t)h << 16);
}

static inline uint16_t fetch_word(uint16_t address, uint8_t bank) {
    const uint8_t l = fetch6502(address, bank);
    const uint8_t h = fetch6502((uint16_t)(address + 1), bank);
    return (uint16_t)(l | (h << 8));
}

static inline uint32_t fetch_long(uint16_t address, uint8_t bank) {
    const uint8_t l = fetch6502(address, bank);
    const uint8_t m = fetch6502((uint16_t)(address + 1), bank);
    const uint8_t h = fetch6502((uint16_t)(address + 2), bank);
    return (uint32_t)l | ((uint32_t)m << 8) | ((uint32_t)h << 16);
}

uint16_t pull16() {
    incsp();
    uint16_t temp16 = read6502(regs.sp, 0);
    incsp();
    temp16 |= (uint16_t) read6502(regs.sp, 0) << 8;
    return temp16;
}

uint8_t pull8() {
    incsp();
    uint8_t value = read6502(regs.sp, 0);
    return value;
}

// The instructions the 65C816 added use the whole stack pointer even in
// emulation mode, where the ones inherited from the 6502 wrap inside page one.
// The high byte is forced back to $01 once the instruction finishes, so only
// the addresses touched during it tell the two apart.
void push16_long(uint16_t pushval) {
    write6502(regs.sp, 0, (pushval >> 8) & 0xFF);
    regs.sp--;
    write6502(regs.sp, 0, pushval & 0xFF);
    regs.sp--;
}

void push8_long(uint8_t pushval) {
    write6502(regs.sp, 0, pushval);
    regs.sp--;
}

uint16_t pull16_long() {
    regs.sp++;
    uint16_t temp16 = read6502(regs.sp, 0);
    regs.sp++;
    temp16 |= (uint16_t) read6502(regs.sp, 0) << 8;
    return temp16;
}

uint8_t pull8_long() {
    regs.sp++;
    return read6502(regs.sp, 0);
}

// Emulation mode keeps the stack pointer in page one. Applied after each
// instruction rather than inside the pushes, so a 16-bit access can cross the
// boundary and come back.
static inline void
cpu_pin_stack_page(void)
{
    if (regs.e) {
        regs.sp = 0x0100 | (regs.sp & 0x00FF);
    }
}

void reset6502(bool c816) {
    cpu_irq_ctx_reset();   // any handler in flight is abandoned by a reset
    regs.pc = read_pair(0xFFFC, 0xFFFD, 0);
    regs.c = 0;
    regs.x = 0;
    regs.y = 0;
    regs.dp = 0;
    regs.sp = 0x1FD;
    regs.e = 1;
    regs.k = 0;
    regs.db = 0;
    if (c816) {
        regs.status |= FLAG_INDEX_WIDTH | FLAG_MEMORY_WIDTH;
        regs.is65c816 = true;
        ticktable = ticktable_c816;
        optable = optable_c816;
        addrtable = addrtable_c816;
    } else {
        regs.status |= FLAG_CONSTANT;
        regs.is65c816 = false;
        ticktable = ticktable_c02;
        optable = optable_c02;
        addrtable = addrtable_c02;
    }
    setinterrupt();
    cleardecimal();
    waiting = 0;
}

enum InterruptType {
    INT_COP = 0x4,
    INT_BRK = 0x6,
    INT_NMI = 0xA,
    INT_IRQ = 0xE
};

void interrupt6502(enum InterruptType vector) {
    // BRK and COP are opcodes, so their cycles come from the tick table. A
    // hardware interrupt is not dispatched through the table and is charged
    // here instead. Recorded before the BRK vector is rewritten to IRQ below.
    const bool from_opcode = (vector == INT_BRK || vector == INT_COP);
    const uint16_t sp0 = regs.sp;
    const int entered_vector = (int)vector;

    // Tell the debugger an interrupt is being taken, before anything is pushed:
    // regs.pc is still the interrupted address and regs.sp is what the matching
    // RTI will restore. See cpu/irq_ctx.h.
    cpu_irq_ctx_enter((int)vector, regs.pc | ((uint32_t)regs.k << 16), regs.sp);

    if (!regs.e) {
        push8(regs.k);
    }

    regs.k = 0; // also in emulated mode

    push16(regs.pc);

    if (regs.e) {
        // BRK and COP are software interrupts and push P with the break flag
        // set; IRQ and NMI push it clear. The flag only exists in the pushed
        // copy, which is what distinguishes the two on return.
        if (vector == INT_BRK || vector == INT_COP) {
            push8(regs.status | FLAG_BREAK);
            vector = (vector == INT_BRK) ? INT_IRQ : vector;
        } else {
            push8(regs.status & ~FLAG_BREAK);
        }
    } else {
        push8(regs.status);
    }

    setinterrupt();
    cleardecimal();
    vp6502();

    uint16_t vector_address = (regs.e ? 0xFFF0 : 0xFFE0) + (uint8_t) vector;
    regs.pc = read_pair(vector_address, vector_address + 1, 0);

    if (!from_opcode) {
        clockticks6502 += 7; // consumed by CPU to process interrupt
    }
    if (cpu_flow_hook) cpu_flow_hook(CPU_FLOW_IRQ_ENTER, regs.pc, sp0, (uint16_t)entered_vector);
}

// Opcodes that take the indexing cycle whether or not the index carries: stores
// on both CPUs, INC and DEC abs,X on both, and the shifts abs,X on the 65C816.
static inline bool index_cycle_is_unconditional(void) {
    switch (opcode) {
        case 0x91: case 0x99: case 0x9D: case 0x9E:
        case 0xDE: case 0xFE:
            return true;
        case 0x1E: case 0x3E: case 0x5E: case 0x7E:
            return regs.is65c816;
        default:
            return false;
    }
}

// The indexing cycle. The 65C02 re-reads the last operand byte; the 65C816
// presents the address before the carry, with VDA and VPA low.
static inline void index_dummy(uint32_t base, uint16_t last_operand) {
    const bool crossed = (base & 0xFFFF00) != (ea & 0xFFFF00);
    if (!crossed && !index_16bit() && !index_cycle_is_unconditional()) {
        return;
    }
    if (regs.is65c816) {
        dummy6502((uint16_t)((base & 0xFF00) | (ea & 0x00FF)), bank_byte(base), false, 0);
    } else {
        dummy6502(last_operand, regs.k, false, 0);
    }
}

// Direct page not on a page boundary costs a cycle at the operand byte.
static inline void dp_low_dummy(void) {
    if (regs.dp & 0x00FF) {
        dummy6502((uint16_t)(regs.pc - 1), regs.k, false, 0);
    }
}

// The cycle adding an index to a direct-page operand. The 65C02 reads the
// unindexed zero-page address; the 65C816 repeats the operand address.
static inline void dp_index_dummy(uint16_t operand) {
    if (regs.is65c816) {
        dummy6502((uint16_t)(regs.pc - 1), regs.k, false, 0);
    } else {
        dummy6502((uint16_t)(operand & 0xFF), 0, false, 0);
    }
}

static uint32_t addr_with_db(uint16_t addr) {
    return mask_long_addr(as_bank_byte(regs.db) | addr);
}

static uint32_t addr_with_k(uint16_t addr) {
    return mask_long_addr(as_bank_byte(regs.k) | addr);
}
