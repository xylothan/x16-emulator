"""Mutations for the CPU's memory map and bus."""

MUTATIONS = [
    {
        "name": "VIA is not told a read is for the debugger",
        "file": "src/memory.c",
        "find": "return via1_read(address & 0xf, debugOn);",
        "into": "return via1_read(address & 0xf, false);",
        "count": 1,
        "caught_by": ["debugon_contract"],
    },
    {
        "name": "a debug read of slow I/O costs cycles",
        "file": "src/memory.c",
        # The write path guards alike, so the pattern includes the read's continuation.
        "find": "if (!debugOn && address >= 0x9fa0) {\n\t\t// slow IO5-7 range\n\t\tclockticks6502 += 3;\n\t}\n\tif (address >= 0x9f00 && address < 0x9f10) {",
        "into": "if (address >= 0x9fa0) {\n\t\t// slow IO5-7 range\n\t\tclockticks6502 += 3;\n\t}\n\tif (address >= 0x9f00 && address < 0x9f10) {",
        "count": 1,
        "caught_by": ["debugon_contract"],
    },
    {
        "name": "a watchpoint swallows the store",
        "file": "src/memory.c",
        # Anchored on the write check: the read paths break the same way.
        "find": "debug_wp_check_write(address, value)) {\n\t\tDEBUGBreakOnWatchpoint();\n\t}",
        "into": "debug_wp_check_write(address, value)) {\n\t\tDEBUGBreakOnWatchpoint();\n\t\treturn;\n\t}",
        "count": 1,
        "caught_by": ["watchpoint_purity"],
    },
    {
        "name": "a dummy bus cycle is not counted",
        "file": "src/memory.c",
        # SingleStepTests wdc65c02/v1 $EE: the write is cycle 5.
        "find": "(void)real_read6502(address, 0, false, USE_CURRENT_X16_BANK);\n\t\t}\n\t}\n\tcpu_bus_index++;",
        "into": "(void)real_read6502(address, 0, false, USE_CURRENT_X16_BANK);\n\t\t}\n\t}",
        "count": 1,
        "caught_by": ["cpu_access_cycles"],
    },
    {
        "name": "an access's cycle counts from its own opcode",
        "file": "src/memory.c",
        "find": "return (unsigned)(clockticks6502 + cpu_bus_index - cpu_step_base);",
        "into": "return (unsigned)cpu_bus_index;",
        "count": 1,
        "caught_by": ["cpu_access_cycles"],
    },
    {
        "name": "a VIA read is not brought up to its cycle",
        "file": "src/memory.c",
        "find": "\t\tif (!debugOn) via1_catch_up(cpu_access_offset());\n\t\treturn via1_read",
        "into": "\t\treturn via1_read",
        "count": 1,
        "caught_by": ["cpu_access_cycles"],
    },
    {
        "name": "banked RAM window is 4K instead of 8K",
        "file": "src/memory.c",
        # x16-docs Memory Map: "$A000-$BFFF Banked RAM (8 KB window into one of
        # 256 banks for a total of 2 MB)". A short stride overlaps every bank
        # with the top half of the one below it.
        "find": "return BRAM[(ramBank << 13) + address - 0xa000];",
        "into": "return BRAM[(ramBank << 12) + address - 0xa000];",
        "count": 1,
        "caught_by": ["memory_banking"],
    },
    {
        "name": "banked RAM writes ignore the bank",
        "file": "src/memory.c",
        # The window is the point: every bank would otherwise be the same 8K,
        # and a write through one bank would be visible through all of them.
        "find": "BRAM[(memory_get_ram_bank() << 13) + address - 0xa000] = value;",
        "into": "BRAM[address - 0xa000] = value;",
        "count": 1,
        "caught_by": ["memory_banking"],
    },
    {
        "name": "the RAM bank register is masked to the installed banks",
        "file": "src/memory.c",
        # x16-docs Memory Map: "$0000 Current RAM bank (0-255)". Masking on the
        # way in would make a bank beyond the installed RAM alias a real one
        # instead of reaching nothing.
        "find": "case 0:\n\t\t\tmemory_set_ram_bank(value);",
        "into": "case 0:\n\t\t\tmemory_set_ram_bank(value & 0x3f);",
        "count": 1,
        "caught_by": ["memory_banking"],
    },
    {
        "name": "an uninstalled RAM bank reads as zero",
        "file": "src/memory.c",
        # No oracle says what an uninstalled bank returns; the test records
        # this emulator's answer as characterization. The mutation is here so
        # that record cannot quietly stop describing anything.
        "find": "\t\t\treturn (address >> 8) & 0xff; // open bus read\n\t\t}\n\t} else { // banked ROM",
        "into": "\t\t\treturn 0;\n\t\t}\n\t} else { // banked ROM",
        "count": 1,
        "caught_by": ["memory_banking"],
    },
    {
        "name": "a debug write charges the slow-I/O cycles",
        "file": "src/memory.c",
        # A debugger's edit updates the machine and does nothing else, so it
        # spends no cycles -- the same reason a debug read spends none. Losing
        # the guard makes using the debugger change program timing.
        "find": "\t\t\t// slow IO2 range\n\t\t\tif (!debugOn) {\n\t\t\t\tclockticks6502 += 3;\n\t\t\t}\n\t\t\t// Both ports are partially decoded.",
        "into": "\t\t\t// slow IO2 range\n\t\t\tclockticks6502 += 3;\n\t\t\t// Both ports are partially decoded.",
        "count": 1,
        "caught_by": ["debug_write_path"],
    },
    {
        "name": "a program write to slow I/O is free",
        "file": "src/memory.c",
        # The other side of the same guard: an executed store still costs cycles.
        "find": "\t\tif (!debugOn && address >= 0x9fa0) {\n\t\t\t// slow IO5-7 range\n\t\t\tclockticks6502 += 3;\n\t\t}\n\t\tif (address >= 0x9f00 && address < 0x9f10) {",
        "into": "\t\tif (false && address >= 0x9fa0) {\n\t\t\t// slow IO5-7 range\n\t\t\tclockticks6502 += 3;\n\t\t}\n\t\tif (address >= 0x9f00 && address < 0x9f10) {",
        "count": 1,
        "caught_by": ["debug_write_path"],
    },
    {
        "name": "a debug write reports a watchpoint",
        "file": "src/memory.c",
        # A watchpoint must not break on the debugger's own edit.
        "find": "if (!debugOn && debug_wp_armed_cpu_write > 0 && bank == 0 && debug_wp_check_write(address, value)) {",
        "into": "if (debug_wp_armed_cpu_write > 0 && bank == 0 && debug_wp_check_write(address, value)) {",
        "count": 1,
        "caught_by": ["debug_write_path"],
    },
    {
        "name": "a VERA read is not brought up to its cycle",
        "file": "src/memory.c",
        "find": "\t\t\tvideo_catch_up(cpu_access_offset());\n\t\t\taudio_catch_up",
        "into": "\t\t\taudio_catch_up",
        "count": 1,
        "caught_by": ["cpu_access_cycles"],
    },
    {
        "name": "a VERA write is not brought up to its cycle",
        "file": "src/memory.c",
        "find": "\t\t\t\tvideo_catch_up(cpu_access_offset());\n",
        "into": "",
        "count": 1,
        "caught_by": ["cpu_access_cycles"],
    },
    {
        "name": "a YM write is not brought up to its cycle",
        "file": "src/memory.c",
        "find": "\t\t\tif (!debugOn && !headless) audio_catch_up(cpu_access_offset());\n\t\t\taudio_render();",
        "into": "\t\t\taudio_render();",
        "count": 1,
        "caught_by": ["cpu_access_cycles"],
    },
    {
        "name": "a YM status read is not brought up to its cycle",
        "file": "src/memory.c",
        "find": "\t\t\t\tif (!headless) audio_catch_up(cpu_access_offset());\n",
        "into": "",
        "count": 1,
        "caught_by": ["cpu_access_cycles"],
    },
    {
        "name": "a VERA write does not bring the sound chips up to its cycle",
        "file": "src/memory.c",
        "find": "\t\t\t\taudio_catch_up(cpu_access_offset());\n\t\t\t}\n\t\t\tvideo_set_debug_write",
        "into": "\t\t\t}\n\t\t\tvideo_set_debug_write",
        "count": 1,
        "caught_by": ["cpu_access_cycles"],
    },
    {
        "name": "a VERA read does not bring the sound chips up to its cycle",
        "file": "src/memory.c",
        "find": "\t\t\tvideo_catch_up(cpu_access_offset());\n\t\t\taudio_catch_up(cpu_access_offset());\n\t\t}\n\t\treturn video_read",
        "into": "\t\t\tvideo_catch_up(cpu_access_offset());\n\t\t}\n\t\treturn video_read",
        "count": 1,
        "caught_by": ["cpu_access_cycles"],
    },
    {
        "name": "a VERA write is caught up after it lands",
        "file": "src/memory.c",
        "find": "\t\t\tif (!debugOn && !headless) {\n\t\t\t\tvideo_catch_up(cpu_access_offset());\n\t\t\t\taudio_catch_up(cpu_access_offset());\n\t\t\t}\n\t\t\tvideo_set_debug_write(debugOn);\n\t\t\tvideo_write(address & 0x1f, value);\n",
        "into": "\t\t\tvideo_set_debug_write(debugOn);\n\t\t\tvideo_write(address & 0x1f, value);\n\t\t\tif (!debugOn && !headless) {\n\t\t\t\tvideo_catch_up(cpu_access_offset());\n\t\t\t\taudio_catch_up(cpu_access_offset());\n\t\t\t}\n",
        "count": 1,
        "caught_by": ["cpu_access_cycles"],
    },
    {
        "name": "a YM write is caught up after it lands",
        "file": "src/memory.c",
        "find": "\t\t\tif (!debugOn && !headless) audio_catch_up(cpu_access_offset());\n\t\t\taudio_render();\n\t\t\tif (port == 0) {\n\t\t\t\taddr_ym = value;\n\t\t\t}\n\t\t\tif (YM_write(port, value)) {",
        "into": "\t\t\taudio_render();\n\t\t\tif (port == 0) {\n\t\t\t\taddr_ym = value;\n\t\t\t}\n\t\t\tconst bool ym_lost = YM_write(port, value);\n\t\t\tif (!debugOn && !headless) audio_catch_up(cpu_access_offset());\n\t\t\tif (ym_lost) {",
        "count": 1,
        "caught_by": ["cpu_access_cycles"],
    },
]

