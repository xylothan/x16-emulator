"""Mutations for the VIAs, I2C, the SMC and the RTC."""

MUTATIONS = [
    {
        "name": "VIA IER reads back without bit 7",
        "file": "src/via.c",
        # W65C22S p27, Table 2-12 note 3: "If a read of this register is done,
        # bit 7 will be Logic 1 and all other bits will reflect their
        # enable/disable state."
        "find": "return via->registers[14] | 0x80;",
        "into": "return via->registers[14];",
        "count": 1,
        "caught_by": ["via_irq"],
    },
    {
        "name": "VIA IER set and clear are the wrong way round",
        "file": "src/via.c",
        # W65C22S p26: bit 7 of the written value selects between setting and
        # clearing the enables. Inverted, every attempt to disable an interrupt
        # enables it instead.
        "find": "if (value & 0x80) {",
        "into": "if (!(value & 0x80)) {",
        "count": 1,
        "caught_by": ["via_irq"],
    },
    {
        "name": "VIA IFR bit 7 ignores the enables",
        "file": "src/via.c",
        # W65C22S p26: IRQ = IFR6 & IER6 | IFR5 & IER5 | ... | IFR0 & IER0.
        # Dropping the enable half reports an interrupt for a flag nobody asked
        # to be told about.
        "find": "irq = (ifr & via->registers[14]) != 0;",
        "into": "irq = ifr != 0;",
        "count": 1,
        "caught_by": ["via_irq"],
    },
    {
        "name": "VIA debug read of T1C-L acknowledges the timer",
        "file": "src/via.c",
        # W65C22S p27, Table 2-11: reading T1C-L low clears the T1 flag. That
        # is a side effect the debugger must not perform, or opening a memory
        # view on $9F14 acknowledges the guest's timer interrupt for it.
        "find": "if (!debug) via->registers[13] &= ~0x40;",
        "into": "via->registers[13] &= ~0x40;",
        "count": 1,
        "caught_by": ["via_irq"],
    },
    {
        "name": "VIA T1 one-shot and free-run are swapped",
        "file": "src/via.c",
        # W65C22S p17-18: ACR bit 6 clear is one-shot, "a single Interrupt Flag
        # each time the Timer is loaded"; set is free-run, where the flag is
        # raised at every zero with no reload.
        "find": "if (!(acr & 0x40)) {\n\t\t\t\tvia->timer_running[0] = false;",
        "into": "if (acr & 0x40) {\n\t\t\t\tvia->timer_running[0] = false;",
        "count": 1,
        "caught_by": ["via_timers"],
    },
    {
        "name": "VIA T1 loads its low byte from the write, not the latch",
        "file": "src/via.c",
        # W65C22S p17: "the microprocessor does not write directly into the T1
        # low order counter. Instead, this half of the counter is loaded
        # automatically from the low order register when the microprocessor
        # writes into the high order register and counter."
        "find": "via->timer_count[0] = ((unsigned)value << 8) | via->registers[6];",
        "into": "via->timer_count[0] = ((unsigned)value << 8) | value;",
        "count": 1,
        "caught_by": ["via_timers"],
    },
    {
        "name": "VIA T2 counts the clock in pulse mode",
        "file": "src/via.c",
        # W65C22S p19: T2 "operates in the One-Shot Mode only (as an interval
        # timer), or as a pulse counter for counting negative pulses on PB6. A
        # single control bit within ACR5 is used to select between these two
        # modes."
        "find": "\tif (acr & 0x20) {\n\t\tn = via->pb6_pulse_counts;",
        "into": "\tif (false) {\n\t\tn = via->pb6_pulse_counts;",
        "count": 1,
        "caught_by": ["via_timers"],
    },
    {
        "name": "VIA T2 re-arms itself after timing out",
        "file": "src/via.c",
        # W65C22S p19: T2 has no free-run mode, so its flag is raised once per
        # load. Leaving it armed turns a one-shot into a repeating interrupt.
        "find": "\t\tifr |= 0x20;\n\t\tvia->timer_running[1] = false;",
        "into": "\t\tifr |= 0x20;",
        "count": 1,
        "caught_by": ["via_timers"],
    },
    {
        "name": "VIA T1 decrements in its write cycle",
        "file": "src/via.c",
        # W65C22S Figure 2-3: the count is N in the cycle after the T1C-H write.
        "find": "if (n && via->timer_loaded[0]) {",
        "into": "if (false && via->timer_loaded[0]) {",
        "count": 1,
        "caught_by": ["via_timers", "via_accuracy"],
    },
    {
        "name": "VIA T2 decrements in its write cycle",
        "file": "src/via.c",
        # W65C22S Figure 2-3, T2 COUNT.
        "find": "if (n && via->timer_loaded[1]) {",
        "into": "if (false && via->timer_loaded[1]) {",
        "count": 1,
        "caught_by": ["via_timers", "via_accuracy"],
    },
    {
        "name": "VIA T1 times out a cycle early",
        "file": "src/via.c",
        # W65C22S Figure 2-3: FFFF follows 0, N+1.5 cycles after the write.
        "find": "n -= cnt + 1;",
        "into": "n -= cnt;",
        "count": 1,
        "caught_by": ["via_timers", "via_accuracy"],
    },
    {
        "name": "VIA skips free-run periods of N+1 cycles",
        "file": "src/via.c",
        # W65C22S Figure 2-4: N+2 cycles per period.
        "find": "const unsigned period = latch + 2;",
        "into": "const unsigned period = latch + 1;",
        "count": 1,
        "caught_by": ["via_accuracy"],
    },
    {
        "name": "VIA steps an access's cycles again at the instruction's end",
        "file": "src/via.c",
        "find": "via_step(via, clocks > via->consumed ? clocks - via->consumed : 0);",
        "into": "via_step(via, clocks);",
        "count": 1,
        "caught_by": ["via_accuracy"],
    },
    {
        "name": "I2C STOP is only seen one bit into a byte",
        "file": "src/i2c.c",
        # DS20005010J 4.1.1.3: SDA rising while SCL is high, at any point.
        "find": "\t\t} else {\n\t\t\tbus_stop();",
        "into": "\t\t} else if (bus == BUS_RX && bits == 1) {\n\t\t\tbus_stop();",
        "count": 1,
        "caught_by": ["i2c_targets"],
    },
    {
        "name": "I2C repeated START is ignored",
        "file": "src/i2c.c",
        # DS20005010J 4.1.1.2, 6.1.4.
        "find": "\tend_transaction();\n\tbus = BUS_RX;",
        "into": "\tif (bus != BUS_IDLE && bus != BUS_IGNORE) return;\n\tend_transaction();\n\tbus = BUS_RX;",
        "count": 1,
        "caught_by": ["i2c_targets"],
    },
    {
        "name": "RTC VBATEN is taken from bit 5",
        "file": "src/rtc.c",
        # DS20005010J Register 5-4: VBATEN is bit 3.
        "find": "0xff, 0x7f, 0x7f, 0x0f,",
        "into": "0xff, 0x7f, 0x7f, 0x27,",
        "count": 1,
        "caught_by": ["i2c_targets"],
    },
    {
        "name": "RTC pointer does not advance on a read",
        "file": "src/rtc.c",
        # DS20005010J 6.1.3, 6.1.5.
        "find": "const uint8_t v = reg_read(pointer);\n\tadvance();",
        "into": "const uint8_t v = reg_read(pointer);",
        "count": 1,
        "caught_by": ["i2c_targets"],
    },
    {
        "name": "RTC SRAM pointer does not wrap to 0x20",
        "file": "src/rtc.c",
        # DS20005010J 6.1.5: 0x5F rolls over to 0x20.
        "find": "pointer = pointer == 0x5f ? 0x20 : pointer + 1;",
        "into": "pointer = pointer == 0x5f ? 0x5f : pointer + 1;",
        "count": 1,
        "caught_by": ["i2c_targets"],
    },
    {
        "name": "RTC register pointer runs on into SRAM",
        "file": "src/rtc.c",
        # DS20005010J 6.1.2: 0x1F rolls over to 0x00.
        "find": "pointer = (pointer + 1) & 0x1f;",
        "into": "pointer = pointer + 1;",
        "count": 1,
        "caught_by": ["i2c_targets"],
    },
    {
        "name": "RTC alarm flag is not set again while matching",
        "file": "src/rtc.c",
        # DS20005010J 5.4 note 1.
        "find": "rtcc[a] = v & writable[a];\n\talarms();",
        "into": "rtcc[a] = v & writable[a];",
        "count": 1,
        "caught_by": ["i2c_targets"],
    },
    {
        "name": "RTC 12-hour clock shows 0 for 12",
        "file": "src/rtc.c",
        # DS20005010J Register 5-3.
        "find": "const int h = v % 12 ? v % 12 : 12;",
        "into": "const int h = v % 12;",
        "count": 1,
        "caught_by": ["i2c_targets"],
    },
    {
        "name": "RTC LPYR is not reported",
        "file": "src/rtc.c",
        # DS20005010J Register 5-6.
        "find": "return v | (leap_year() ? LPYR : 0);",
        "into": "return v;",
        "count": 1,
        "caught_by": ["i2c_targets"],
    },
    {
        "name": "SMC acts on a write before STOP",
        "file": "src/smc.c",
        # x16-smc r48.0.0 smc_wire.cpp: I2C_Receive runs from the START/STOP interrupt.
        "find": "\t\trx[rx_len++] = v;\n\t\treturn true;",
        "into": "\t\trx[rx_len++] = v;\n\t\tif (rx_len >= 2) smc_receive();\n\t\treturn true;",
        "count": 1,
        "caught_by": ["i2c_targets"],
    },
    {
        "name": "SMC acts on a write of the command byte alone",
        "file": "src/smc.c",
        # x16-smc r48.0.0 x16-smc.ino I2C_Receive: fewer than two bytes return early.
        "find": "if (ct < 2) {\n\t\treturn;",
        "into": "if (ct < 1) {\n\t\treturn;",
        "count": 1,
        "caught_by": ["i2c_targets"],
    },
    {
        "name": "SMC acknowledges an empty reply",
        "file": "src/smc.c",
        # x16-smc r48.0.0 smc_wire.cpp VERIFY_ADDRESS: buflen 0 is ignored.
        "find": "return tx_len > 0;",
        "into": "return true;",
        "count": 1,
        "caught_by": ["i2c_targets"],
    },
    {
        "name": "SMC reads past its reply as 0",
        "file": "src/smc.c",
        # x16-smc r48.0.0 smc_wire.cpp SEND_DATA: $FF past the buffer.
        "find": "return tx_idx < tx_len ? tx[tx_idx++] : 0xff;",
        "into": "return tx_idx < tx_len ? tx[tx_idx++] : 0x00;",
        "count": 1,
        "caught_by": ["i2c_targets"],
    },
    {
        "name": "SMC passes on an overflowed mouse packet",
        "file": "src/smc.c",
        # x16-smc r48.0.0 x16-smc.ino sendMousePacket.
        "find": "if (!(first & 0xc0)) {",
        "into": "if (true) {",
        "count": 1,
        "caught_by": ["i2c_targets"],
    },
    {
        "name": "SMC reset leaves the long press",
        "file": "src/smc.c",
        # x16-smc r48.0.0 x16-smc.ino DoReset.
        "find": "if (powered) {\n\t\tpwr_long_press = false;",
        "into": "if (false) {\n\t\tpwr_long_press = false;",
        "count": 1,
        "caught_by": ["i2c_targets"],
    },
    {
        "name": "VIA CA1 is active on the other edge",
        "file": "src/via.c",
        "find": "level != via->ca1 && level == ((pcr & 0x01) != 0)",
        "into": "level != via->ca1 && level != ((pcr & 0x01) != 0)",
        "count": 1,
        "caught_by": ["via_accuracy"],
    },
    {
        "name": "VIA CB1 is active on the other edge",
        "file": "src/via.c",
        "find": "level != via->cb1 && level == ((pcr & 0x10) != 0)",
        "into": "level != via->cb1 && level != ((pcr & 0x10) != 0)",
        "count": 1,
        "caught_by": ["via_accuracy"],
    },
    {
        "name": "VIA CA2 sets its flag as an output",
        "file": "src/via.c",
        "find": "level != via->ca2 && !(pcr & 0x08) && ",
        "into": "level != via->ca2 && ",
        "count": 1,
        "caught_by": ["via_accuracy"],
    },
    {
        "name": "VIA CA2 takes its edge from the independent bit",
        "file": "src/via.c",
        "find": "level == ((pcr & 0x04) != 0)",
        "into": "level == ((pcr & 0x02) != 0)",
        "count": 1,
        "caught_by": ["via_accuracy"],
    },
    {
        "name": "VIA CB2 takes its edge from the independent bit",
        "file": "src/via.c",
        "find": "level == ((pcr & 0x40) != 0)",
        "into": "level == ((pcr & 0x20) != 0)",
        "count": 1,
        "caught_by": ["via_accuracy"],
    },
    {
        "name": "VIA CA1 latches nothing",
        "file": "src/via.c",
        "find": "\t\t\t\tvia->ira = via->pa_pins;\n",
        "into": "",
        "count": 1,
        "caught_by": ["via_accuracy"],
    },
    {
        "name": "VIA CB1 latches nothing",
        "file": "src/via.c",
        "find": "\t\t\t\tvia->irb = via->pb_pins;\n",
        "into": "",
        "count": 1,
        "caught_by": ["via_accuracy"],
    },
    {
        "name": "VIA latching starts from the last latched value",
        "file": "src/via.c",
        "find": "if (value & ~via->registers[11] & 0x01) via->ira = via->pa_pins;\n",
        "into": "",
        "count": 1,
        "caught_by": ["via_accuracy"],
    },
    {
        "name": "VIA CA1 leaves the CA2 handshake low",
        "file": "src/via.c",
        "find": "\t\t\t\tvia->ca2_hs = false;\n",
        "into": "",
        "count": 1,
        "caught_by": ["via_accuracy"],
    },
    {
        "name": "VIA CB1 leaves the CB2 handshake low",
        "file": "src/via.c",
        "find": "\t\t\t\tvia->cb2_hs = false;\n",
        "into": "",
        "count": 1,
        "caught_by": ["via_accuracy"],
    },
    {
        "name": "VIA CA2 pulses for no cycle",
        "file": "src/via.c",
        "find": "via->ca2_pulse = 2;",
        "into": "via->ca2_pulse = 1;",
        "count": 1,
        "caught_by": ["via_accuracy"],
    },
    {
        "name": "VIA CB2 pulses for two cycles",
        "file": "src/via.c",
        "find": "via->cb2_pulse = 2;",
        "into": "via->cb2_pulse = 3;",
        "count": 1,
        "caught_by": ["via_accuracy"],
    },
    {
        "name": "VIA CA2 handshake output inverted",
        "file": "src/via.c",
        "find": "case 4: return !hs;",
        "into": "case 4: return hs;",
        "count": 1,
        "caught_by": ["via_accuracy"],
    },
    {
        "name": "VIA IRB reads the pins on output bits",
        "file": "src/via.c",
        "find": "return (via->registers[0] & via->registers[2]) | (via_irb(via) & ~via->registers[2]);",
        "into": "return via_irb(via);",
        "count": 1,
        "caught_by": ["via_accuracy"],
    },
    {
        "name": "VIA IRA reads the pins on output bits",
        "file": "src/via.c",
        "find": "return (via->registers[1] & via->registers[3]) | (via_ira(via) & ~via->registers[3]);",
        "into": "return via_ira(via);",
        "count": 1,
        "caught_by": ["via_accuracy"],
    },
]
