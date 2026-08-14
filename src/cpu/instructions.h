/*

						Extracted from original single fake6502.c file

*/
//
//          65C02 changes.
//
//          BRK                 now clears D
//          ADC/SBC             set N and Z in decimal mode. They also set V
//
//
//
//          instruction handler functions
//
static void adc() {
    pay_for_wide_memory(1);
    penaltyop = 1;
    if (regs.status & FLAG_DECIMAL) {
        uint16_t tmp, tmp2;
        uint32_t tmpov;
        if (memory_16bit()) {
            uint16_t tmp3;
            uint32_t tmp4;
            value = getvalue(1);
            tmp = (regs.c & 0x000F) + (value & 0x000F) + (uint16_t)(regs.status & FLAG_CARRY);
            tmp2 = (regs.c & 0x00F0) + (value & 0x00F0);
            tmp3 = (regs.c & 0x0F00) + (value & 0x0F00);
            tmp4 = ((uint32_t)regs.c & 0xF000) + (value & 0xF000);
            if (tmp > 0x0009) {
                tmp2 += 0x0010;
                tmp += 0x0006;
            }
            if (tmp2 > 0x0090) {
                tmp3 += 0x0100;
                tmp2 += 0x0060;
            }
            if (tmp3 > 0x0900) {
                tmp4 += 0x1000;
                tmp3 += 0x0600;
            }
            tmpov = tmp4;
            if (tmp4 > 0x9000) {
                tmp4 += 0x6000;
            }
            if (tmp4 & 0xFFFF0000) {
                setcarry();
            } else {
                clearcarry();
            }
            result = (tmp & 0x000F) | (tmp2 & 0x00F0) | (tmp3 & 0x0F00) | (tmp4 & 0xF000);
            uint16_t ovresult = (tmp & 0x000F) | (tmp2 & 0x00F0) | (tmp3 & 0x0F00) | (tmpov & 0xF000);
            overflowcalc16(ovresult, regs.c, value);
        } else {
            value = getvalue(0);
            tmp = ((uint16_t)regs.a & 0x0F) + (value & 0x0F) + (uint16_t)(regs.status & FLAG_CARRY);
            tmp2 = ((uint16_t)regs.a & 0xF0) + (value & 0xF0);
            if (tmp > 0x09) {
                tmp2 += 0x10;
                tmp += 0x06;
            }
            tmpov = tmp2;
            if (tmp2 > 0x90) {
                tmp2 += 0x60;
            }
            if (tmp2 & 0xFF00) {
                setcarry();
            } else {
                clearcarry();
            }
            result = (tmp & 0x0F) | (tmp2 & 0xF0);
            uint8_t ovresult = (tmp & 0x0F) | (tmpov & 0xF0);
            overflowcalc8((uint16_t)ovresult, (uint16_t)regs.a, value);
        }
        clockticks6502 += (uint32_t)(!regs.is65c816);
    } else {
        if (memory_16bit()) {
            value = getvalue(1);
            result = regs.c + value + (uint16_t) (regs.status & FLAG_CARRY);
            overflowcalc16(result, regs.c, value);
            carrycalc(result, 1);
        } else {
            value = getvalue(0);
            result = (uint16_t)regs.a + value + (uint16_t) (regs.status & FLAG_CARRY);
            overflowcalc8(result, (uint16_t)regs.a, value);
            carrycalc(result, 0);
        }
    }

    zerocalc(result, memory_16bit());
    signcalc(result, memory_16bit());

    saveaccum(result);
}

static void and() {
    pay_for_wide_memory(1);
    penaltyop = 1;
    value = getvalue(memory_16bit());
    result = acc_for_mode() & value;

    zerocalc(result, memory_16bit());
    signcalc(result, memory_16bit());

    saveaccum(result);
}

static void asl() {
    pay_for_wide_memory(2);
    // On the 65C02 the page-crossing cycle is conditional here; the NMOS
    // part always paid it and the 65C816 has its own flat timing, so this
    // penalty applies to the 65C02 only.
    penaltyop = (uint8_t)!regs.is65c816;
    value = getvalue(memory_16bit());
    result = value << 1;

    carrycalc(result, memory_16bit());
    zerocalc(result, memory_16bit());
    signcalc(result, memory_16bit());

    putvalue(result, memory_16bit());
}

static void _do_branch(int condition) {
    if (condition) {
        oldpc = regs.pc;
        regs.pc += reladdr;
        clockticks6502++;
        if ((oldpc & 0xFF00) != (regs.pc & 0xFF00)) //check if jump crossed a page boundary
            penaltye = 1;
    }
}

static void bcc() {
    _do_branch((regs.status & FLAG_CARRY) == 0);
}

static void bcs() {
    _do_branch((regs.status & FLAG_CARRY) == FLAG_CARRY);
}

static void beq() {
    _do_branch((regs.status & FLAG_ZERO) == FLAG_ZERO);
}

static void bit() {
    pay_for_wide_memory(1);
    // BIT abs,X pays the page-crossing cycle on both CPUs, unlike the
    // read-modify-write forms below it, which are flat on the 65C816.
    penaltyop = 1;
    value = getvalue(memory_16bit());
    result = acc_for_mode() & value;

    zerocalc(result, memory_16bit());
    // Xark - BUGFIX: 65C02 BIT #$xx only affects Z  See: http://6502.org/tutorials/65c02opcodes.html#2
    if (opcode != 0x89)
    {
        // N and V come from the top two bits of the operand, which are bits 15
        // and 14 when the accumulator is 16 bits wide.
        uint16_t top = memory_16bit() ? (uint16_t)(value >> 8) : value;
        regs.status = (regs.status & 0x3F) | (uint8_t)(top & 0xC0);
    }
}

static void bmi() {
    _do_branch((regs.status & FLAG_SIGN) == FLAG_SIGN);
}

static void bne() {
    _do_branch((regs.status & FLAG_ZERO) == 0);
}

static void bpl() {
    _do_branch((regs.status & FLAG_SIGN) == 0);
}

static void brk() {
    penaltyn = 1;
    regs.pc++;

    interrupt6502(INT_BRK);
}

static void brl() {
    regs.pc += reladdr;
}

static void bvc() {
    _do_branch((regs.status & FLAG_OVERFLOW) == 0);
}

static void bvs() {
    _do_branch((regs.status & FLAG_OVERFLOW) == FLAG_OVERFLOW);
}

static void clc() {
    clearcarry();
}

static void cld() {
    cleardecimal();
}

static void cli() {
    clearinterrupt();
}

static void clv() {
    clearoverflow();
}

static void cmp() {
    pay_for_wide_memory(1);
    penaltyop = 1;
    value = getvalue(memory_16bit());

    if (memory_16bit()) {
        result = regs.c - value;
        if (regs.c >= value) setcarry();
        else clearcarry();
        if (regs.c == value) setzero();
        else clearzero();
    } else {
        result = (uint16_t)regs.a - value;
        if (regs.a >= (uint8_t)(value & 0x00FF)) setcarry();
        else clearcarry();
        if (regs.a == (uint8_t)(value & 0x00FF)) setzero();
        else clearzero();
    }

    signcalc(result, memory_16bit());
}

static void cop() {
    penaltyn = 1;
    regs.pc++;

    interrupt6502(INT_COP);
}

static void cpx() {
    penaltyx = 1;
    value = getvalue(index_16bit());

    if (index_16bit()) {
        result = regs.x - value;
        if(regs.x >= value) setcarry();
        else clearcarry();
        if (regs.x == value) setzero();
        else clearzero();
    } else {
        result = (uint16_t)regs.xl - value;
        if (regs.xl >= (uint8_t)(value & 0x00FF)) setcarry();
        else clearcarry();
        if (regs.xl == (uint8_t)(value & 0x00FF)) setzero();
        else clearzero();
    }
    signcalc(result, index_16bit());
}

static void cpy() {
    penaltyx = 1;
    value = getvalue(index_16bit());

    if (index_16bit()) {
        result = regs.y - value;
        if(regs.y >= value) setcarry();
        else clearcarry();
        if (regs.y == value) setzero();
        else clearzero();
    } else {
        result = (uint16_t)regs.yl - value;
        if (regs.yl >= (uint8_t)(value & 0x00FF)) setcarry();
        else clearcarry();
        if (regs.yl == (uint8_t)(value & 0x00FF)) setzero();
        else clearzero();
    }
    signcalc(result, index_16bit());
}

static void dec() {
    pay_for_wide_memory(2);
    value = getvalue(memory_16bit());
    result = value - 1;

    zerocalc(result, memory_16bit());
    signcalc(result, memory_16bit());

    putvalue(result, memory_16bit());
}

static void dex() {
    if (index_16bit()) {
        regs.x--;
        zerocalc(regs.x, 1);
        signcalc(regs.x, 1);
    } else {
        regs.xl--;
        zerocalc(regs.xl, 0);
        signcalc(regs.xl, 0);
    }
}

static void dey() {
    if (index_16bit()) {
        regs.y--;
        zerocalc(regs.y, 1);
        signcalc(regs.y, 1);
    } else {
        regs.yl--;
        zerocalc(regs.yl, 0);
        signcalc(regs.yl, 0);
    }
}

static void eor() {
    pay_for_wide_memory(1);
    penaltyop = 1;
    value = getvalue(memory_16bit());
    result = acc_for_mode() ^ value;

    zerocalc(result, memory_16bit());
    signcalc(result, memory_16bit());

    saveaccum(result);
}

static void inc() {
    pay_for_wide_memory(2);
    value = getvalue(memory_16bit());
    result = value + 1;

    zerocalc(result, memory_16bit());
    signcalc(result, memory_16bit());

    putvalue(result, memory_16bit());
}

static void inx() {
    if (index_16bit()) {
        regs.x++;
        zerocalc(regs.x, 1);
        signcalc(regs.x, 1);
    } else {
        regs.xl++;
        zerocalc(regs.xl, 0);
        signcalc(regs.xl, 0);
    }
}

static void iny() {
    if (index_16bit()) {
        regs.y++;
        zerocalc(regs.y, 1);
        signcalc(regs.y, 1);
    } else {
        regs.yl++;
        zerocalc(regs.yl, 0);
        signcalc(regs.yl, 0);
    }
}

static void jml() {
    regs.pc = ea & 0xFFFF;
    regs.k = ea >> 16;
}

static void jmp() {
    regs.pc = ea;
}

static void jsr() {
    push16(regs.pc - 1);
    regs.pc = ea;
}

static void jsl() {
    push8_long(regs.k);
    push16_long(regs.pc - 1);
    regs.pc = ea & 0xFFFF;
    regs.k = ea >> 16;
}

static void lda() {
    penaltyop = 1;
    penaltym = 1;

    if (memory_16bit()) {
        regs.c = getvalue(1);
        zerocalc(regs.c, 1);
        signcalc(regs.c, 1);
    } else {
        value = getvalue(0);
        regs.a = (uint8_t)(value & 0x00FF);
        zerocalc(regs.a, 0);
        signcalc(regs.a, 0);
    }
}

static void ldx() {
    penaltyop = 1;
    penaltyx = 1;

    if (index_16bit()) {
        regs.x = getvalue(1);
        zerocalc(regs.x, 1);
        signcalc(regs.x, 1);
    } else {
        value = getvalue(0);
        regs.xl = (uint8_t)(value & 0x00FF);
        zerocalc(regs.xl, 0);
        signcalc(regs.xl, 0);
    }
}

static void ldy() {
    penaltyop = 1;
    penaltyx = 1;

    if (index_16bit()) {
        regs.y = getvalue(1);
        zerocalc(regs.y, 1);
        signcalc(regs.y, 1);
    } else {
        regs.yl = (uint8_t)(getvalue(0) & 0x00FF);
        zerocalc(regs.yl, 0);
        signcalc(regs.yl, 0);
    }
}

static void lsr() {
    pay_for_wide_memory(2);
    // On the 65C02 the page-crossing cycle is conditional here; the NMOS
    // part always paid it and the 65C816 has its own flat timing, so this
    // penalty applies to the 65C02 only.
    penaltyop = (uint8_t)!regs.is65c816;
    value = getvalue(memory_16bit());
    result = value >> 1;

    if (value & 1) setcarry();
        else clearcarry();
    zerocalc(result, memory_16bit());
    signcalc(result, memory_16bit());

    putvalue(result, memory_16bit());
}

static void nop() {
    switch (opcode) {
        case 0x1C:
        case 0x3C:
        case 0x5C:
        case 0x7C:
        case 0xDC:
        case 0xFC:
            penaltyop = 1;
            break;
    }
}

static void ora() {
    pay_for_wide_memory(1);
    penaltyop = 1;
    value = getvalue(memory_16bit());
    result = acc_for_mode() | value;

    zerocalc(result, memory_16bit());
    signcalc(result, memory_16bit());

    saveaccum(result);
}

static void pea() {
    push16_long(getvalue(1));
}

static void pei() {
    push16_long(ea);
}

static void per() {
    push16_long(regs.pc + reladdr);
}

static void pha() {
    pay_for_wide_memory(1);
    if (memory_16bit()) {
        push16(regs.c);
    } else {
        push8(regs.a);
    }
}

static void phb() {
    push8_long(regs.db);
}

static void phd() {
    push16_long(regs.dp);
}

static void phk() {
    push8_long(regs.k);
}

static void php() {
    push8(regs.e ? regs.status | FLAG_BREAK : regs.status);
}

static void pla() {
    pay_for_wide_memory(1);
    if (memory_16bit()) {
        regs.c = pull16();
        zerocalc(regs.c, 1);
        signcalc(regs.c, 1);
    } else {
        regs.a = pull8();
        zerocalc(regs.a, 0);
        signcalc(regs.a, 0);
    }
}

static void plb() {
    regs.db = pull8_long();
    zerocalc(regs.db, 0);
    signcalc(regs.db, 0);
}

static void pld() {
    regs.dp = pull16_long();
    zerocalc(regs.dp, 1);
    signcalc(regs.dp, 1);
}

static void plp() {
    regs.status = cpu_merge_pulled_status(pull8());
    if (regs.is65c816) {
        if (regs.e) {
            regs.status |= FLAG_INDEX_WIDTH | FLAG_MEMORY_WIDTH;
        } else if (regs.status & FLAG_INDEX_WIDTH) {
            regs.xh = 0;
            regs.yh = 0;
        }
    }
}

static void rep() {
    value = getvalue(0);
    regs.status &= ~(value & 0xFF);

    if (regs.e) {
        regs.status |= FLAG_INDEX_WIDTH | FLAG_MEMORY_WIDTH;
    }
}

static void rol() {
    pay_for_wide_memory(2);
    // On the 65C02 the page-crossing cycle is conditional here; the NMOS
    // part always paid it and the 65C816 has its own flat timing, so this
    // penalty applies to the 65C02 only.
    penaltyop = (uint8_t)!regs.is65c816;
    value = getvalue(memory_16bit());
    result = (value << 1) | (regs.status & FLAG_CARRY);

    carrycalc(result, memory_16bit());
    zerocalc(result, memory_16bit());
    signcalc(result, memory_16bit());

    putvalue(result, memory_16bit());
}

static void ror() {
    pay_for_wide_memory(2);
    // On the 65C02 the page-crossing cycle is conditional here; the NMOS
    // part always paid it and the 65C816 has its own flat timing, so this
    // penalty applies to the 65C02 only.
    penaltyop = (uint8_t)!regs.is65c816;
    value = getvalue(memory_16bit());
    result = (value >> 1) | ((regs.status & FLAG_CARRY) << (memory_16bit() ? 15 : 7));

    if (value & 1) setcarry();
        else clearcarry();
    zerocalc(result, memory_16bit());
    signcalc(result, memory_16bit());

    putvalue(result, memory_16bit());
}

static void rti() {
    // Native mode pulls the program bank as well, which costs a cycle.
    penaltyn = 1;
    regs.status = cpu_merge_pulled_status(pull8());
    value = pull16();
    regs.pc = value;

    if (regs.is65c816) {
        if (regs.e) {
            regs.status |= FLAG_INDEX_WIDTH | FLAG_MEMORY_WIDTH;
        } else {
            if (regs.status & FLAG_INDEX_WIDTH) {
                regs.xh = 0;
                regs.yh = 0;
            }
            regs.k = pull8();
        }
    }

    // Pair this with the interrupt entry that set the stack up. Reported after
    // every pull, so regs.sp is back to what it was when the interrupt was
    // taken. See cpu/irq_ctx.h.
    cpu_irq_ctx_leave(regs.sp);
}

static void rtl() {
    value = pull16_long();
    regs.pc = value + 1;
    regs.k = pull8_long();
}

static void rts() {
    value = pull16();
    regs.pc = value + 1;
}

static void sbc() {
    pay_for_wide_memory(1);
    penaltyop = 1;

    if (regs.status & FLAG_DECIMAL) {
        if (memory_16bit()) {
            value = getvalue(1);
            const uint16_t carry_in = regs.status & FLAG_CARRY;

            // Four digits, each corrected when the one below borrows. Signed
            // throughout: the top digit corrects on a borrow out, matching the
            // 8-bit path, where testing the digit against $A instead left the
            // result $6000 low whenever a high digit was $A-$F.
            int32_t d0 = (int32_t)(regs.c & 0x000F) - (int32_t)(value & 0x000F) +
                         (int32_t)carry_in - 1;
            int32_t d1 = (int32_t)(regs.c & 0x00F0) - (int32_t)(value & 0x00F0);
            int32_t d2 = (int32_t)(regs.c & 0x0F00) - (int32_t)(value & 0x0F00);
            int32_t d3 = (int32_t)(regs.c & 0xF000) - (int32_t)(value & 0xF000);

            if (d0 & ~0x000F) {
                d1 -= 0x0010;
                d0 -= 0x0006;
            }
            if (d1 & ~0x00FF) {
                d2 -= 0x0100;
                d1 -= 0x0060;
            }
            if (d2 & ~0x0FFF) {
                d3 -= 0x1000;
                d2 -= 0x0600;
            }

            int32_t uncorrected = d3;
            if (d3 < 0) {
                d3 -= 0x6000;
            }

            result = (uint16_t)((d0 & 0x000F) | (d1 & 0x00F0) | (d2 & 0x0F00) |
                                (d3 & 0xF000));
            uint16_t c_result = (uint16_t)((d0 & 0x000F) | (d1 & 0x00F0) |
                                           (d2 & 0x0F00) | (uncorrected & 0xF000));

            if (c_result <= regs.c) {
                setcarry();
            } else {
                clearcarry();
            }
            uint16_t ovresult = regs.c + (value ^ 0xFFFF) + carry_in;
            overflowcalc16(ovresult, regs.c, value ^ 0xFFFF);
        } else {
            value = getvalue(0);
            // Captured before setcarry() below, which would otherwise feed the
            // freshly computed carry back into the overflow calculation.
            const uint16_t carry_in = regs.status & FLAG_CARRY;

            // The 65C02 subtracts in binary and corrects afterwards; the 65C816
            // corrects each nibble as it goes, as the NMOS part did. The two
            // agree on valid BCD and differ when a digit is $A-$F.
            int16_t lo  = (int16_t)(regs.a & 0x0F) - (int16_t)(value & 0x0F) +
                          (int16_t)carry_in - 1;
            int16_t bin = (int16_t)regs.a - (int16_t)value +
                          (int16_t)carry_in - 1;

            int16_t adjusted;
            if (regs.is65c816) {
                int16_t al = lo;
                if (al < 0) {
                    al = (int16_t)(((al - 0x06) & 0x0F) - 0x10);
                }
                adjusted = (int16_t)((int16_t)(regs.a & 0xF0) -
                                     (int16_t)(value & 0xF0) + al);
                if (adjusted < 0) {
                    adjusted -= 0x60;
                }
            } else {
                adjusted = bin;
                if (adjusted < 0) {
                    adjusted -= 0x60;
                }
                if (lo < 0) {
                    adjusted -= 0x06;
                }
            }
            result = (uint16_t)(adjusted & 0xFF);

            uint8_t ovresult = (uint8_t)(regs.a + (value ^ 0xFF) + carry_in);
            overflowcalc8((uint16_t)ovresult, (uint16_t)regs.a, value ^ 0xFF);

            // Carry reports the binary borrow, not the corrected result.
            if (bin >= 0) {
                setcarry();
            } else {
                clearcarry();
            }
        }

        clockticks6502 += (uint32_t)(!regs.is65c816);
    } else {
        if (memory_16bit()) {
            value = getvalue(1) ^ 0xFFFF;
            result = regs.c + value + (regs.status & FLAG_CARRY);
            overflowcalc16(result, regs.c, value);
        } else {
            value = getvalue(0) ^ 0x00FF;
            result = (uint16_t)regs.a + value + (uint16_t)(regs.status & FLAG_CARRY);
            overflowcalc8(result, (uint16_t)regs.a, value);
        }

        carrycalc(result, memory_16bit());
    }

    zerocalc(result, memory_16bit());
    signcalc(result, memory_16bit());

    saveaccum(result);
}

static void sec() {
    setcarry();
}

static void sed() {
    setdecimal();
}

static void sei() {
    setinterrupt();
}

static void sep() {
    regs.status |= getvalue(0) & 0xFF;
    if (regs.e) {
        regs.status |= FLAG_INDEX_WIDTH | FLAG_MEMORY_WIDTH;
    }
    if (regs.status & FLAG_INDEX_WIDTH) {
        regs.xh = 0;
        regs.yh = 0;
    }
}

static void sta() {
    pay_for_wide_memory(1);
    putvalue(acc_for_mode(), memory_16bit());
}

static void stx() {
    penaltyx = 1;
    putvalue(index_16bit() ? regs.x : regs.xl, index_16bit());
}

static void sty() {
    penaltyx = 1;
    putvalue(index_16bit() ? regs.y : regs.yl, index_16bit());
}

static void tax() {
    if (index_16bit()) {
        regs.x = regs.c; // 16 bits transferred, no matter the state of m
        zerocalc(regs.x, 1);
        signcalc(regs.x, 1);
    } else {
        regs.xl = (uint8_t)(regs.a & 0x00FF);
        zerocalc(regs.xl, 0);
        signcalc(regs.xl, 0);
    }
}

static void tay() {
    if (index_16bit()) {
        regs.y = regs.c; // 16 bits transferred, no matter the state of m
        zerocalc(regs.y, 1);
        signcalc(regs.y, 1);
    } else {
        regs.yl = (uint8_t)(regs.a & 0x00FF);
        zerocalc(regs.yl, 0);
        signcalc(regs.yl, 0);
    }
}

static void tcd() {
    regs.dp = regs.c;
    zerocalc(regs.dp, 1);
    signcalc(regs.dp, 1);
}

static void tdc() {
    regs.c = regs.dp;
    zerocalc(regs.c, 1);
    signcalc(regs.c, 1);
}

static void tsx() {
    if (index_16bit()) {
        regs.x = regs.sp; // 16 bits transferred, no matter the state of m
        zerocalc(regs.x, 1);
        signcalc(regs.x, 1);
    } else {
        regs.xl = (uint8_t)(regs.sp & 0x00FF);
        regs.xh = 0;
        zerocalc(regs.xl, 0);
        signcalc(regs.xl, 0);
    }
}

static void txa() {
    if (memory_16bit()) {
        // The transfer is the accumulator's width, so the flags are too, even
        // where an 8-bit X supplies only the low byte.
        if (index_16bit()) {
            regs.c = regs.x;
        } else {
            regs.a = regs.xl;
            regs.b = 0;
        }
        zerocalc(regs.c, 1);
        signcalc(regs.c, 1);
    } else {
        regs.a = regs.xl;
        zerocalc(regs.a, 0);
        signcalc(regs.a, 0);
    }
}

static void txs() {
    if (regs.e) {
        regs.sp = 0x100 | regs.xl;
    } else {
        regs.sp = regs.x;
    }
}

static void txy() {
    if (index_16bit()) {
        regs.y = regs.x;
        zerocalc(regs.y, 1);
        signcalc(regs.y, 1);
    } else {
        regs.yl = regs.xl;
        zerocalc(regs.yl, 0);
        signcalc(regs.yl, 0);
    }
}

static void tya() {
    if (memory_16bit()) {
        if (index_16bit()) {
            regs.c = regs.y;
        } else {
            regs.a = regs.yl;
            regs.b = 0;
        }
        zerocalc(regs.c, 1);
        signcalc(regs.c, 1);
    } else {
        regs.a = regs.yl;
        zerocalc(regs.a, 0);
        signcalc(regs.a, 0);
    }
}

static void tyx() {
    if (index_16bit()) {
        regs.x = regs.y;
        zerocalc(regs.x, 1);
        signcalc(regs.x, 1);
    } else {
        regs.xl = regs.yl;
        zerocalc(regs.xl, 0);
        signcalc(regs.xl, 0);
    }
}

static void tcs() {
    regs.sp = regs.c;
}

static void tsc() {
    regs.c = regs.sp;
    zerocalc(regs.c, 1);
    signcalc(regs.c, 1);
}

static void mvn() {
    uint8_t sourceBank = ea >> 8;
    uint8_t destBank = ea;
    regs.db = destBank;

    if (index_16bit()) {
        write6502(regs.y++, destBank, read6502(regs.x++, sourceBank));
    } else {
        write6502(regs.yl++, destBank, read6502(regs.xl++, sourceBank));
    }
    if (--regs.c != 0xFFFF) {
        regs.pc -= 3;
    }
}

static void mvp() {
    uint8_t sourceBank = ea >> 8;
    uint8_t destBank = ea;
    regs.db = ea;

    if (index_16bit()) {
        write6502(regs.y--, destBank, read6502(regs.x--, sourceBank));
    } else {
        write6502(regs.yl--, destBank, read6502(regs.xl--, sourceBank));
    }
    if (--regs.c != 0xFFFF) {
        regs.pc -= 3;
    }
}

static void wdm() {
}

static void xba() {
    uint8_t tmp = regs.b;
    regs.b = regs.a;
    regs.a = tmp;
    zerocalc(regs.a, 0);
    signcalc(regs.a, 0);
}

static void xce() {
    uint8_t carry = regs.status & FLAG_CARRY;
    regs.status = (regs.status & ~FLAG_CARRY) | (regs.e ? FLAG_CARRY : 0);
    regs.e = carry != 0;

    if (regs.e) {
        regs.status |= FLAG_INDEX_WIDTH | FLAG_MEMORY_WIDTH;
        regs.sp = 0x0100 | (regs.sp & 0x00FF);
        regs.xh = 0x00;
        regs.yh = 0x00;
    }
}
