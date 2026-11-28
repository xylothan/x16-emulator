"""Mutations for the 65C02 and 65C816 cores."""

MUTATIONS = [
    {
        "name": "direct-page cycle penalty is never cleared",
        "file": "src/cpu/fake6502.c",
        # step6502()'s copy, four-space indented; exec6502()'s is eight-space and dead code.
        "find": "\n    penaltyd = 0;\n",
        "into": "\n",
        "count": 1,
        "caught_by": ["cpu_smoke"],
    },
    {
        "name": "page-cross penalty is dropped",
        "file": "src/cpu/fake6502.c",
        "find": "if (penaltyop && penaltyaddr) clockticks6502++;",
        "into": "if (0) clockticks6502++;",
        "count": 2,  # exec6502() and step6502() both have it
        # Not caught by klaus: it checks results, not cycle counts.
        "caught_by": ["cpu_loadstore"],
    },
    {
        "name": "decimal mode costs no extra cycle",
        "file": "src/cpu/instructions.h",
        "find": "clockticks6502 += (uint32_t)(!regs.is65c816);",
        "into": "clockticks6502 += (uint32_t)(0);",
        "count": 2,  # adc() and sbc()
        "caught_by": ["cpu_decimal"],
    },
    {
        "name": "the overflow flag is never set",
        "file": "src/cpu/support.h",
        # Mask with 0x00, not `if (0)`: macro parameters stay referenced under -Werror.
        "find": "if (((n) ^ (m)) & ((n) ^ (o)) & 0x80) setoverflow();",
        "into": "if (((n) ^ (m)) & ((n) ^ (o)) & 0x00) setoverflow();",
        "count": 1,
        "caught_by": ["cpu_adc_sbc", "klaus"],
    },
    {
        "name": "zero page indexing does not wrap inside page zero",
        "file": "src/cpu/support.h",
        "find": "((uint16_t) ((uint8_t) (regs.dp & 0x00FF)) + (offset & 0xFF))",
        "into": "((uint16_t) ((uint8_t) (regs.dp & 0x00FF)) + offset)",
        "count": 1,
        "caught_by": ["cpu_loadstore", "klaus"],
    },
    # The three below check ProcessorTests: a baseline comparison must move when behaviour moves.
    {
        "name": "the negative flag is never set",
        "file": "src/cpu/support.h",
        "find": "else if ((n) & 0x0080) setsign();",
        "into": "else if ((n) & 0x0000) setsign();",
        "count": 1,
        "requires": "tests/pt/wdc65c02.bin",
        "caught_by": ["processor_tests", "klaus"],
    },
    {
        "name": "the zero flag is set from the wrong width",
        "file": "src/cpu/support.h",
        "find": "else if ((n) & 0x00FF) clearzero();",
        "into": "else if ((n) & 0x000F) clearzero();",
        "count": 1,
        "requires": "tests/pt/wdc65c02.bin",
        "caught_by": ["processor_tests", "klaus"],
    },
    {
        # Timing only: results stay correct, so klaus cannot see it.
        "name": "a taken branch costs no extra cycle",
        "file": "src/cpu/instructions.h",
        "find": "clockticks6502++;",
        "into": "clockticks6502 += 0;",
        "count": 1,
        "requires": "tests/pt/wdc65c02.bin",
        "caught_by": ["processor_tests"],
    },
    # The 65C816 fixes below, each checked by the suite for its mode.
    {
        "name": "indirect addressing ignores the data bank",
        "file": "src/cpu/modes.h",
        "find": "ea = addr_with_db(read_pair(direct_page_add(eahelp),",
        "into": "ea = (read_pair(direct_page_add(eahelp),",
        "count": 2,  # ind0 and indx
        "requires": "tests/pt/816-emu.bin",
        "caught_by": ["processor_tests_816_emu"],
    },
    {
        "name": "the new stack instructions wrap inside page one",
        "file": "src/cpu/instructions.h",
        "find": "regs.dp = pull16_long();",
        "into": "regs.dp = pull16();",
        "count": 1,
        "requires": "tests/pt/816-emu.bin",
        "caught_by": ["processor_tests_816_emu"],
    },
    {
        "name": "a 16-bit operand costs no extra cycle",
        "file": "src/cpu/support.h",
        "find": "(penaltym = addressing_is_acc ? 0 : (uint8_t)(n))",
        "into": "(penaltym = addressing_is_acc ? 0 : (uint8_t)((n) & 0x00))",
        "count": 1,
        "requires": "tests/pt/816-native.bin",
        "caught_by": ["processor_tests_816_native"],
    },
    {
        "name": "BIT reads the wrong half of a 16-bit operand",
        "file": "src/cpu/instructions.h",
        "find": "uint16_t top = memory_16bit() ? (uint16_t)(value >> 8) : value;",
        "into": "uint16_t top = value;",
        "count": 1,
        "requires": "tests/pt/816-native.bin",
        "caught_by": ["processor_tests_816_native"],
    },
    {
        # The two CPUs correct decimal subtraction differently.
        "name": "the 65C02 decimal SBC algorithm is used on the 65C816",
        "file": "src/cpu/instructions.h",
        "find": "            int16_t adjusted;\n            if (regs.is65c816) {",
        "into": "            int16_t adjusted;\n            if (0) {",
        "count": 1,
        "requires": "tests/pt/816-emu.bin",
        "caught_by": ["processor_tests_816_emu"],
    },
    {
        # WAI: ProcessorTests cannot compare it, so only opcode_spec sees this change.
        "name": "a documented cycle count is changed",
        "file": "src/cpu/65c02.opcodes",
        "find": "wai imp 3 $cb",
        "into": "wai imp 4 $cb",
        "count": 1,
        "caught_by": ["opcode_spec"],
    },
    {
        "name": "an implied instruction makes no second access",
        "file": "src/cpu/modes.h",
        # SingleStepTests wdc65c02/v1 $18.
        "find": "    pc_dummy(regs.pc);\n}\n\nstatic void imp8()",
        "into": "}\n\nstatic void imp8()",
        "count": 1,
        "requires": "tests/pt/wdc65c02.bin",
        "caught_by": ["processor_tests"],
    },
    {
        "name": "a 65C02 one-cycle NOP makes a second access",
        "file": "src/cpu/modes.h",
        # SingleStepTests wdc65c02/v1 $0B.
        "find": "(opcode & 0x07) == 0x03",
        "into": "(opcode & 0x0F) == 0x03",
        "count": 1,
        "requires": "tests/pt/wdc65c02.bin",
        "caught_by": ["processor_tests"],
    },
    {
        "name": "BRK does not fetch its signature byte",
        "file": "src/cpu/modes.h",
        # SingleStepTests wdc65c02/v1 $00.
        "find": "    // The signature byte is fetched and discarded.\n    pc_dummy(regs.pc);",
        "into": "",
        "count": 1,
        "requires": "tests/pt/wdc65c02.bin",
        "caught_by": ["processor_tests"],
    },
    {
        "name": "the 65C02 reads nothing before a pull",
        "file": "src/cpu/support.h",
        # SingleStepTests wdc65c02/v1 $68.
        "find": "dummy6502((uint16_t)(0x0100 | (regs.sp & 0xFF)), 0, false, 0);",
        "into": "(void)0;",
        "count": 1,
        "requires": "tests/pt/wdc65c02.bin",
        "caught_by": ["processor_tests"],
    },
    {
        "name": "the 65C816 reads the stack before a pull",
        "file": "src/cpu/support.h",
        # SingleStepTests 65816/v1 $68.
        "find": "    if (regs.is65c816) {\n        pc_dummy(regs.pc);\n    } else {",
        "into": "    if (regs.is65c816) {\n        dummy6502(regs.sp, 0, false, 0);\n    } else {",
        "count": 1,
        "requires": "tests/pt/816-native.bin",
        "caught_by": ["processor_tests_816_native", "cpu_dummy_cycles"],
    },
    {
        "name": "a taken 65C02 branch makes no access",
        "file": "src/cpu/support.h",
        # SingleStepTests wdc65c02/v1 $D0.
        "find": "        pc_dummy(from);\n        if (page_cycle) {",
        "into": "        if (page_cycle) {",
        "count": 1,
        "requires": "tests/pt/wdc65c02.bin",
        "caught_by": ["processor_tests"],
    },
    {
        "name": "a page-crossing 65C02 branch re-reads the next opcode",
        "file": "src/cpu/support.h",
        # SingleStepTests wdc65c02/v1 $80.
        "find": "pc_dummy((uint16_t)((from & 0xFF00) | (regs.pc & 0x00FF)));",
        "into": "pc_dummy(from);",
        "count": 1,
        "requires": "tests/pt/wdc65c02.bin",
        "caught_by": ["processor_tests"],
    },
    {
        "name": "a taken 65C816 branch reads the next opcode",
        "file": "src/cpu/support.h",
        # SingleStepTests 65816/v1 $D0.
        "find": "    if (regs.is65c816) {\n        pc_dummy((uint16_t)(from - 1));",
        "into": "    if (regs.is65c816) {\n        pc_dummy(from);",
        "count": 1,
        "requires": "tests/pt/816-emu.bin",
        "caught_by": ["processor_tests_816_emu"],
    },
    {
        "name": "65C02 JSR fetches its high byte before the pushes",
        "file": "src/cpu/modes.h",
        # SingleStepTests wdc65c02/v1 $20.
        "find": "    ea = fetch6502(regs.pc++, regs.k);\n    if (regs.is65c816) {\n        ea |=",
        "into": "    ea = fetch6502(regs.pc++, regs.k);\n    if (1) {\n        ea |=",
        "count": 1,
        "requires": "tests/pt/wdc65c02.bin",
        "caught_by": ["processor_tests"],
    },
    {
        "name": "65C02 JSR reads nothing before the pushes",
        "file": "src/cpu/instructions.h",
        # SingleStepTests wdc65c02/v1 $20.
        "find": "    if (!regs.is65c816) {\n        pull_dummy();\n    }",
        "into": "",
        "count": 1,
        "requires": "tests/pt/wdc65c02.bin",
        "caught_by": ["processor_tests"],
    },
    {
        "name": "65C02 RTS does not read the pulled address",
        "file": "src/cpu/instructions.h",
        # SingleStepTests wdc65c02/v1 $60.
        "find": "        pc_dummy((uint16_t)value);",
        "into": "",
        "count": 1,
        "requires": "tests/pt/wdc65c02.bin",
        "caught_by": ["processor_tests"],
    },
    {
        "name": "65C816 RTS makes no access after the pulls",
        "file": "src/cpu/instructions.h",
        # SingleStepTests 65816/v1 $60.
        "find": "        dummy6502(regs.sp, 0, false, 0);\n    } else {\n        pc_dummy((uint16_t)value);",
        "into": "    } else {\n        pc_dummy((uint16_t)value);",
        "count": 1,
        "requires": "tests/pt/816-native.bin",
        "caught_by": ["processor_tests_816_native", "cpu_access_cycles", "cpu_dummy_cycles"],
    },
    {
        "name": "JSL makes no access after pushing the program bank",
        "file": "src/cpu/instructions.h",
        # SingleStepTests 65816/v1 $22.
        "find": "    dummy6502((uint16_t)(regs.sp + 1), 0, false, 0);\n",
        "into": "",
        "count": 1,
        "requires": "tests/pt/816-native.bin",
        "caught_by": ["processor_tests_816_native", "cpu_dummy_cycles"],
    },
    {
        "name": "JSR (abs,X) makes no access after the high byte",
        "file": "src/cpu/instructions.h",
        # SingleStepTests 65816/v1 $FC.
        "find": "        pc_dummy(regs.pc);\n        const uint16_t pointer",
        "into": "        const uint16_t pointer",
        "count": 1,
        "requires": "tests/pt/816-native.bin",
        "caught_by": ["processor_tests_816_native"],
    },
    {
        "name": "65C02 JMP (abs,X) re-reads the high byte",
        "file": "src/cpu/65c02.h",
        # SingleStepTests wdc65c02/v1 $7C.
        "find": "pc_dummy(regs.is65c816 ? (uint16_t)(regs.pc + 1) : regs.pc);",
        "into": "pc_dummy((uint16_t)(regs.pc + 1));",
        "count": 1,
        "requires": "tests/pt/wdc65c02.bin",
        "caught_by": ["processor_tests"],
    },
    {
        "name": "stack-relative makes no access after the offset",
        "file": "src/cpu/modes.h",
        # SingleStepTests 65816/v1 $A3.
        "find": "(uint16_t)fetch6502(regs.pc++, regs.k));\n    pc_dummy((uint16_t)(regs.pc - 1));",
        "into": "(uint16_t)fetch6502(regs.pc++, regs.k));",
        "count": 1,
        "requires": "tests/pt/816-native.bin",
        "caught_by": ["processor_tests_816_native", "cpu_access_cycles"],
    },
    {
        "name": "(sr,S),Y makes no access after the pointer",
        "file": "src/cpu/modes.h",
        # SingleStepTests 65816/v1 $B3.
        "find": "    dummy6502((uint16_t)(eahelp + 1), 0, false, 0);\n",
        "into": "",
        "count": 1,
        "requires": "tests/pt/816-native.bin",
        "caught_by": ["processor_tests_816_native", "cpu_access_cycles"],
    },
    {
        "name": "PEI makes no access for an unaligned direct page",
        "file": "src/cpu/modes.h",
        # SingleStepTests 65816/v1 $D4.
        "find": "    eahelp = (uint16_t)fetch6502(regs.pc++, regs.k);\n    dp_low_dummy();\n    ea = read_pair(regs.dp",
        "into": "    eahelp = (uint16_t)fetch6502(regs.pc++, regs.k);\n    ea = read_pair(regs.dp",
        "count": 1,
        "requires": "tests/pt/816-native.bin",
        "caught_by": ["processor_tests_816_native", "cpu_access_cycles"],
    },
    {
        "name": "REP makes no access after its operand",
        "file": "src/cpu/instructions.h",
        # SingleStepTests 65816/v1 $C2.
        "find": "    value = getvalue(0);\n    pc_dummy((uint16_t)(regs.pc - 1));\n    regs.status &=",
        "into": "    value = getvalue(0);\n    regs.status &=",
        "count": 1,
        "requires": "tests/pt/816-native.bin",
        "caught_by": ["processor_tests_816_native"],
    },
    {
        "name": "BBR reads its zero-page byte once",
        "file": "src/cpu/65c02.h",
        # SingleStepTests wdc65c02/v1 $0F.
        "find": "\tdummy6502((uint16_t)ea, 0, false, 0);\n\treladdr",
        "into": "\treladdr",
        "count": 1,
        "requires": "tests/pt/wdc65c02.bin",
        "caught_by": ["processor_tests"],
    },
    {
        "name": "a 65C02 NOP zp,X reads the unindexed address",
        "file": "src/cpu/instructions.h",
        # SingleStepTests wdc65c02/v1 $54.
        "find": "dummy6502((uint8_t)(zp + regs.xl), 0, false, 0);",
        "into": "dummy6502(zp, 0, false, 0);",
        "count": 1,
        "requires": "tests/pt/wdc65c02.bin",
        "caught_by": ["processor_tests"],
    },
]
