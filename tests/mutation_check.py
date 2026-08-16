#!/usr/bin/env python3
"""Check that the tests actually fail when the emulator is wrong.

Every test here is written to pass against correct behaviour, which says
nothing about whether it would notice incorrect behaviour. A test that asserts
the wrong thing, or reads a value it never actually exercises, passes exactly
as convincingly as one that works.

So: break the emulator on purpose, one small change at a time, and check the
test that should care goes red. A mutation that survives means either the test
is not covering what it claims, or the code it changed does not matter.

    python tests/mutation_check.py --build-dir build

Every mutation is reverted afterwards, including on failure or interrupt. The
source is restored from a copy taken before each edit, so an aborted run leaves
the tree as it found it.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

CMAKE = os.environ.get("CMAKE", "cmake")
CTEST = os.environ.get("CTEST", "ctest")

# Each entry breaks one specific behaviour and names the tests that should
# notice. `count` is how many places the pattern is expected to match, and a
# mismatch is reported rather than applied -- a pattern that has quietly stopped
# matching would otherwise look exactly like a mutation the tests caught.
MUTATIONS = [
    {
        "name": "PCM FIFO accepts one byte too many",
        "file": "src/vera_pcm.c",
        # audio_fifo.v:27  assign full = (wridx_next == rdidx_r);
        # The ring stores one fewer than its 4096 cells so that full and empty
        # stay distinguishable. Off by one here makes a full FIFO read as empty.
        "find": "if (fifo_cnt < sizeof(fifo) - 1) {",
        "into": "if (fifo_cnt <= sizeof(fifo) - 1) {",
        "count": 1,
        "caught_by": ["vera_pcm"],
    },
    {
        "name": "PCM almost-empty boundary is off by one",
        "file": "src/vera_pcm.c",
        # audio_fifo.v:28  assign almost_empty = fifo_count < 12'd1024;
        # Drives the AFLOW interrupt: firing a byte late or early changes when
        # a program is asked for more audio.
        "find": "return fifo_cnt < 1024;",
        "into": "return fifo_cnt <= 1024;",
        "count": 1,
        "caught_by": ["vera_pcm"],
    },
    # No mutation for the AUDIO_RATE fold. The test asserts the RTL value
    # (top.v:480 stores write_data raw) and marks the emulator's fold as a
    # divergence, so it deliberately does not pin `256 - val`. A mutation of
    # that expression would survive, and correctly so. Add one once the fold is
    # removed and the register stores what was written.
    {
        "name": "VERA debug reads report a constant",
        "file": "src/video.c",
        # The sweep in test_vera_debug_read.c records exactly which registers
        # read differently to the debugger than to the machine. Breaking the
        # debug path further changes that set, which the recorded count fails
        # on -- so the sweep cannot quietly stop measuring anything.
        "find": "if (debugOn) return video_get_dc_value(i);",
        "into": "if (debugOn) return 0;",
        "count": 1,
        "caught_by": ["vera_debug_read"],
    },
    {
        "name": "VERA DCSEL is truncated to four bits",
        "file": "src/video.c",
        # top.v:86 declares dc_select_r as [5:0] and top.v:336 fills it from
        # write_data[6:1]. Narrowing it silently aliases the high banks onto
        # the low ones.
        "find": "io_dcsel = (value >> 1) & 0x3f;",
        "into": "io_dcsel = (value >> 1) & 0x0f;",
        "count": 1,
        "caught_by": ["vera_regbank"],
    },
    {
        "name": "VERA address increment uses a power-of-two step",
        "file": "src/video.c",
        # addr_data.v:193 gives 40 for code 0x0B. The non-power-of-two steps
        # exist so a program can walk tile rows, and are exactly what an
        # implementation "tidying" the series to 1 << n would lose.
        "find": "\t40,  -40,",
        "into": "\t48,  -48,",
        "count": 1,
        "caught_by": ["vera_video"],
    },
    {
        "name": "VERA reads the increment from the wrong bits of ADDRx_H",
        "file": "src/video.c",
        # video.c:2796. Bits 7:3 are the decrement flag and increment code;
        # shifting by one more drops the decrement bit entirely.
        "find": "io_inc[io_addrsel]  = value >> 3;",
        "into": "io_inc[io_addrsel]  = value >> 4;",
        "count": 1,
        "caught_by": ["vera_video"],
    },
    {
        "name": "PSG sawtooth loses its pulse-width shaping",
        "file": "src/vera_psg.c",
        # Reverts the waveform to the R47 form. psg.v:163 (R48) XORs the saw
        # with the inverted pulse width; R47 read the phase directly. This is
        # the mutation that proves pinning psg.v to R48 does any work.
        "find": "case WF_SAWTOOTH: v = (ch->phase >> 11) ^ ((ch->pw ^ 0x3f) & 0x3f); break;",
        "into": "case WF_SAWTOOTH: v = (ch->phase >> 11); break;",
        "count": 1,
        "caught_by": ["vera_psg"],
    },
    {
        "name": "PSG voices keep running while silenced",
        "file": "src/vera_psg.c",
        # psg.v:140 holds a voice at phase zero unless a side is enabled, so
        # unmuting restarts the waveform rather than resuming it.
        "find": "uint32_t new_phase = (ch->left || ch->right) ? ((ch->phase + ch->freq) & 0x1FFFF) : 0;",
        "into": "uint32_t new_phase = ((ch->phase + ch->freq) & 0x1FFFF);",
        "count": 1,
        "caught_by": ["vera_psg"],
    },
    {
        "name": "PSG noise LFSR loses a tap",
        "file": "src/vera_psg.c",
        # psg.v:128 taps 1, 2, 4 and 15. A wrong tap set still sounds like
        # noise, so only the exact sequence catches it.
        "find": "(((noise_state >> 1) ^ (noise_state >> 2) ^ (noise_state >> 4) ^ (noise_state >> 15)) & 1)",
        "into": "(((noise_state >> 1) ^ (noise_state >> 2) ^ (noise_state >> 15)) & 1)",
        "count": 1,
        "caught_by": ["vera_psg"],
    },
    {
        "name": "PSG waveform is decoded from the wrong bits",
        "file": "src/vera_psg.c",
        # psg.v:44 cur_waveform = cur_channel_attr_r[31:30], the top two bits
        # of the fourth attribute byte.
        "find": "channels[ch].waveform = val >> 6;",
        "into": "channels[ch].waveform = (val >> 5) & 3;",
        "count": 1,
        "caught_by": ["vera_psg"],
    },
    {
        "name": "VERA ISR clears on a written zero",
        "file": "src/video.c",
        # top.v:440-442 clear a latch when the written bit is one:
        #   irq_status_vsync_next = irq_status_vsync_r & !write_data[0];
        # Inverting that acknowledges every pending source whenever a program
        # clears one, which loses interrupts rather than reporting them wrong.
        "find": "isr &= value ^ 0xff;",
        "into": "isr &= value;",
        "count": 1,
        "caught_by": ["vera_irq"],
    },
    {
        "name": "VERA IEN stores a fifth enable bit",
        "file": "src/video.c",
        # top.v:175 reads bits 5:4 as 2'b0 and top.v:433-436 give them no write
        # target, so a program cannot store anything there.
        "find": "ien = value & 0xF;",
        "into": "ien = value & 0x1F;",
        "count": 1,
        "caught_by": ["vera_irq"],
    },
    {
        "name": "VERA IRQLINE bit 8 comes from the wrong IEN bit",
        "file": "src/video.c",
        # top.v:432  irq_line_next[8] = write_data[7];
        # The compare value is split across $9F26 and $9F28; taking the high
        # bit from bit 6 puts it where the scanline readback is instead.
        "find": "irq_line = (irq_line & 0xFF) | ((value >> 7) << 8);",
        "into": "irq_line = (irq_line & 0xFF) | (((value >> 6) & 1) << 8);",
        "count": 1,
        "caught_by": ["vera_irq"],
    },
    {
        "name": "VERA ISR bit 3 latches instead of following the FIFO",
        "file": "src/video.c",
        # top.v:176 takes bit 3 from the audio_fifo_low wire, not a latch, so
        # it reports the FIFO level at the instant of the read.
        "find": "case 0x07: return isr | (pcm_is_fifo_almost_empty() ? 8 : 0);",
        "into": "case 0x07: return isr;",
        "count": 1,
        "caught_by": ["vera_irq"],
    },
    {
        "name": "VERA asserts IRQ regardless of the enables",
        "file": "src/video.c",
        # top.v:1200  assign extbus_irq_n = (irq_status & irq_enable) == 0;
        # Dropping the mask interrupts on every VSYNC whether or not the
        # program asked for one.
        "find": "return (tmp_isr & ien) != 0;",
        "into": "return tmp_isr != 0;",
        "count": 1,
        "caught_by": ["vera_irq"],
    },
    {
        "name": "VERA IEN bit 6 reports a stored bit, not the scanline",
        "file": "src/video.c",
        # top.v:175 sources bit 6 from scanline[8], live, with nothing writing
        # it. A debugger reading IEN would otherwise show a raster position
        # that never moves.
        "find": "((scanline & 0x100) >> 2)",
        "into": "0",
        "count": 1,
        "caught_by": ["vera_irq"],
    },
    {
        "name": "VERA collision nibble accumulates across frames",
        "file": "src/video.c",
        # sprite_renderer.v:402  frame_collision_mask_next = cur_collision_mask_r;
        # Assigned unconditionally at frame_done, so the nibble describes the
        # frame just ended. ORing instead makes a collision permanent.
        "find": "isr = (isr & 0xf) | sprite_line_collisions;",
        "into": "isr = isr | sprite_line_collisions;",
        "count": 1,
        "caught_by": ["vera_irq"],
    },
    {
        "name": "VERA sprite collision mask is read from the wrong nibble",
        "file": "src/video.c",
        # sprite_renderer.v:82  wire [3:0] sprite_attr_collision_mask = sprite_attr[23:20];
        # The top half of attribute byte 6; the bottom half is z-depth and the
        # flip bits.
        "find": "props->sprite_collision_mask = sprite_data[sprite][6] & 0xf0;",
        "into": "props->sprite_collision_mask = (sprite_data[sprite][6] & 0x0f) << 4;",
        "count": 1,
        "caught_by": ["vera_irq"],
    },
    {
        "name": "VERA sprites collide where their masks differ",
        "file": "src/video.c",
        # sprite_renderer.v:328 uses the mask directly:
        #   ... ? (linebuf_rddata[15:12] & sprite_collision_mask_r) : 4'b0;
        # It was inverted until 4f46cb32 (2023-01-26). This mutation restores
        # that form, which is the one bug the RTL's own history records here.
        "find": "sprite_line_collisions |= sprite_line_mask[line_x] & props->sprite_collision_mask;",
        "into": "sprite_line_collisions |= sprite_line_mask[line_x] & ~props->sprite_collision_mask;",
        "count": 1,
        "caught_by": ["vera_irq"],
    },
    # No mutation for the ISR write clearing bits 7:4. The test records that as
    # a divergence rather than asserting it, so mutating video.c:2910 towards
    # the RTL would make the marker pass unexpectedly -- which is a failure by
    # design, not a caught mutation. Add one once the write stops reaching the
    # nibble.
    {
        "name": "direct-page cycle penalty is never cleared",
        "file": "src/cpu/fake6502.c",
        # step6502()'s copy, four-space indented. exec6502()'s is eight-space
        # and is dead code -- mutating that proves nothing, which is exactly
        # what an earlier version of this list accidentally did.
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
        # Not klaus: its functional tests check results, not cycle counts, so
        # they are blind to timing. Worth knowing.
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
        "find": "if (!debugOn && address >= 0x9fa0) {",
        "into": "if (address >= 0x9fa0) {",
        "count": 1,
        "caught_by": ["debugon_contract"],
    },
    {
        "name": "a watchpoint swallows the store",
        "file": "src/memory.c",
        "find": "\t\tDEBUGBreakOnWatchpoint();\n\t}",
        "into": "\t\tDEBUGBreakOnWatchpoint();\n\t\treturn;\n\t}",
        "count": 1,
        "caught_by": ["watchpoint_purity"],
    },
    {
        "name": "the overflow flag is never set",
        "file": "src/cpu/support.h",
        # Masking with 0x00 rather than replacing the condition with 0: the
        # macro parameters stay referenced, so this compiles under -Werror
        # where an `if (0)` leaves them unused and GCC refuses.
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
    # The three below exist to check ProcessorTests specifically: a baseline
    # comparison is only worth anything if it moves when behaviour moves.
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
    # The 65C816 fixes below. Each is checked by the suite for the mode it
    # applies to, so a revert shows up as a named opcode rather than a number.
    {
        "name": "indirect addressing ignores the data bank",
        "file": "src/cpu/modes.h",
        "find": "ea = addr_with_db((uint16_t)read6502(direct_page_add(eahelp), 0)",
        "into": "ea = ((uint16_t)read6502(direct_page_add(eahelp), 0)",
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
        # The two CPUs correct decimal subtraction differently; using one
        # algorithm for both was the fault this replaced.
        "name": "the 65C02 decimal SBC algorithm is used on the 65C816",
        "file": "src/cpu/instructions.h",
        "find": "            int16_t adjusted;\n            if (regs.is65c816) {",
        "into": "            int16_t adjusted;\n            if (0) {",
        "count": 1,
        "requires": "tests/pt/816-emu.bin",
        "caught_by": ["processor_tests_816_emu"],
    },
    {
        # WAI is one of the four opcodes ProcessorTests records in a form it
        # cannot compare, so this is a table change no other test can see. It
        # is the mutation that shows opcode_spec covers something new rather
        # than repeating the traces.
        "name": "a documented cycle count is changed",
        "file": "src/cpu/65c02.opcodes",
        "find": "wai imp 3 $cb",
        "into": "wai imp 4 $cb",
        "count": 1,
        "caught_by": ["opcode_spec"],
    },
    {
        "name": "VERA reset keeps the pending sprite collisions",
        "file": "src/video.c",
        # sprite_renderer.v:416  cur_collision_mask_r <= 0;
        # The half of the sprite reset that is right. Dropping it carries
        # collisions from before a reset into the frame after it.
        "find": "\tsprite_line_collisions = 0;\n\n\tvga_scan_pos_x = 0;",
        "into": "\tvga_scan_pos_x = 0;",
        "count": 1,
        "caught_by": ["vera_reset"],
    },
    {
        "name": "VERA sprite z-depth is ignored",
        "file": "src/video.c",
        # sprite_renderer.v:81  wire [1:0] sprite_attr_z = sprite_attr[19:18];
        # A z-depth of zero is not drawn, which is how a guest retires a
        # sprite. test_vera_reset.c measures a reset by whether sprites stop,
        # so this breaks the channel the rest of that file reads.
        "find": "props->sprite_zdepth = (sprite_data[sprite][6] >> 2) & 3;",
        "into": "props->sprite_zdepth = 1;",
        "count": 1,
        "caught_by": ["vera_reset"],
    },
    {
        "name": "VERA layer map base is shifted one bit short",
        "file": "src/video.c",
        # layer_renderer.v:107  wire [14:0] map_addr = {map_baseaddr, 7'b0} + map_idx[15:1];
        # The register names a byte address of register x 512. A shift of 8
        # puts the tilemap at half the offset the guest asked for.
        "find": "props->map_base       = reg_layer[layer][1] << 9;",
        "into": "props->map_base       = reg_layer[layer][1] << 8;",
        "count": 1,
        "caught_by": ["vera_layer"],
    },
    {
        "name": "VERA tile base takes in the two size bits",
        "file": "src/video.c",
        # top.v:383-384 forces tile_baseaddr[1:0] to zero on write; bits 1 and 0
        # of the register are the tile height and width. Letting them into the
        # address moves the tile data whenever the tile size changes.
        "find": "props->tile_base      = (reg_layer[layer][2] & 0xFC) << 9;",
        "into": "props->tile_base      = (reg_layer[layer][2] & 0xFF) << 9;",
        "count": 1,
        "caught_by": ["vera_layer"],
    },
    {
        "name": "VERA map width starts at 16 tiles",
        "file": "src/video.c",
        # layer_renderer.v:100 masks the column index to 5 bits at map_width 0,
        # so the narrowest map is 32 tiles. Starting an octave low halves every
        # map and moves the end of the tilemap with it.
        "find": "props->mapw_log2 = 5 + ((reg_layer[layer][0] >> 4) & 3);",
        "into": "props->mapw_log2 = 4 + ((reg_layer[layer][0] >> 4) & 3);",
        "count": 1,
        "caught_by": ["vera_layer"],
    },
    {
        "name": "VERA layer 1 registers are written to layer 0",
        "file": "src/video.c",
        # top.v:219-225 gives layer 1 its own registers at 5'h14-5'h1A. One
        # bank behind both sets of addresses would leave the two layers sharing
        # a configuration.
        "find": "reg_layer[1][reg - 0x14] = value;",
        "into": "reg_layer[0][reg - 0x14] = value;",
        "count": 1,
        "caught_by": ["vera_layer"],
    },
    # No mutation for the scroll high bytes reading back whole. That is
    # recorded as a divergence rather than asserted, so mutating video.c
    # towards the RTL would make the marker pass unexpectedly -- a failure by
    # design, not a caught mutation.
    {
        "name": "VERA layer row does not wrap at the layer height",
        "file": "src/video.c",
        # layer_renderer.v:86-89 masks the tile row to the map height. Without
        # the mask a tall scroll walks off the end of the tilemap instead of
        # coming back round to the top.
        "find": "return (y + props->vscroll) & (props->layerh_max);",
        "into": "return y + props->vscroll;",
        "count": 1,
        "caught_by": ["vera_layer_rows"],
    },
    {
        "name": "VERA map height starts at 16 tiles",
        "file": "src/video.c",
        # layer_renderer.v:86 masks to 5 bits at map_height 0, so the shortest
        # map is 32 tiles. An octave low halves the wrap point.
        "find": "props->maph_log2 = 5 + ((reg_layer[layer][0] >> 6) & 3);",
        "into": "props->maph_log2 = 4 + ((reg_layer[layer][0] >> 6) & 3);",
        "count": 1,
        "caught_by": ["vera_layer_rows"],
    },
    {
        "name": "VERA tile height is read from the tile width bit",
        "file": "src/video.c",
        # top.v:386-387 takes height from write_data[1] and width from
        # write_data[0]. Swapping them changes which bits of the scrolled line
        # index select the row, per layer_renderer.v:82.
        "find": "props->tileh_log2 = 3 + ((reg_layer[layer][2] >> 1) & 1);",
        "into": "props->tileh_log2 = 3 + (reg_layer[layer][2] & 1);",
        "count": 1,
        "caught_by": ["vera_layer_rows"],
    },
    {
        "name": "VERA default palette entry is wrong",
        "file": "src/video.c",
        # palette_ram.mem is the table baked into the FPGA bitstream, so the
        # default palette is hardware and not a ROM convention. One edited
        # entry in a 256-entry table is how this actually goes wrong, and the
        # only thing that notices is a comparison against the file.
        "find": "0x08f,0xbbb,",
        "into": "0x08f,0xbbc,",
        "count": 1,
        "caught_by": ["vera_palette"],
    },
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
        "find": "if (!(acr & 0x40)) via->timer_running[0] = false;",
        "into": "if (acr & 0x40) via->timer_running[0] = false;",
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
        "find": "tclk = (acr & 0x20) ? via->pb6_pulse_counts : clocks;",
        "into": "tclk = clocks;",
        "count": 1,
        "caught_by": ["via_timers"],
    },
    {
        "name": "VIA T2 re-arms itself after timing out",
        "file": "src/via.c",
        # W65C22S p19: T2 has no free-run mode, so its flag is raised once per
        # load. Leaving it armed turns a one-shot into a repeating interrupt.
        "find": "\t\t\tifr |= 0x20;\n\t\t\tvia->timer_running[1] = false;",
        "into": "\t\t\tifr |= 0x20;",
        "count": 1,
        "caught_by": ["via_timers"],
    },
]


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, **kw)


def build(build_dir, passes=1):
    """Build the tests, optionally more than once.

    A mutation to an .opcodes file regenerates src/cpu/tables.h during the
    build, and the objects that include it are only recompiled on the following
    pass: the header dependency comes from the compiler's depfile, which ninja
    evaluates before the generator has run. One pass would leave the binaries
    linked from the unmutated tables, so such a mutation would look like it
    survived when nothing had actually been tested.
    """
    out = ""
    for _ in range(passes):
        r = run([CMAKE, "--build", build_dir, "--target", "unit_tests"])
        out += r.stdout + r.stderr
        if r.returncode != 0:
            return False, out
    return True, out


def tests_fail(build_dir, names):
    """True if every named test reports failure."""
    for name in names:
        r = run([CTEST, "--test-dir", build_dir, "-R", f"^{name}$"])
        if r.returncode == 0:
            return False, name
    return True, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("--only", help="run just the mutation whose name contains this")
    args = ap.parse_args()

    ok, out = build(args.build_dir)
    if not ok:
        print("the tree does not build before any mutation:\n" + out[-2000:])
        return 2

    survived = []
    not_applicable = []
    for m in MUTATIONS:
        if args.only and args.only not in m["name"]:
            continue

        # Without its fixture the test skips, passes, and looks like the
        # mutation survived.
        need = m.get("requires")
        if need and not (ROOT / need).exists():
            print(f"n/a   {m['name']}\n      needs {need}, which is not present")
            not_applicable.append(m["name"])
            continue

        path = ROOT / m["file"]
        # Universal newlines, so a pattern written with \n matches a file
        # checked out with CRLF. The original bytes are restored afterwards.
        original = path.read_text(encoding="utf-8")

        want = m.get("count", 1)
        count = original.count(m["find"])
        if count != want:
            print(f"SKIP  {m['name']}\n      pattern matches {count} times in "
                  f"{m['file']}, expected {want} -- the source has moved on")
            survived.append(m["name"] + " (pattern stale)")
            continue

        # Building an .opcodes mutation rewrites these, and they are tracked, so
        # they have to be put back with everything else.
        GENERATED = ("src/cpu/tables.h", "src/cpu/mnemonics.h")

        backup = Path(tempfile.gettempdir()) / (path.name + ".mutation_backup")
        shutil.copy2(path, backup)
        gen_backups = {}
        for rel in GENERATED:
            gen = ROOT / rel
            if gen.exists():
                gen_backups[gen] = Path(tempfile.gettempdir()) / (gen.name + ".mutation_backup")
                shutil.copy2(gen, gen_backups[gen])
        try:
            path.write_text(original.replace(m["find"], m["into"]),
                            encoding="utf-8")

            # Delete the generated headers rather than trusting the build to
            # notice they are stale. The generator fires on a timestamp
            # comparison against the .opcodes file, and restoring the previous
            # mutation stamps these to "now" -- so when a whole mutate, build
            # and restore cycle lands inside one filesystem mtime tick, the
            # mutated .opcodes is not newer than tables.h, generation is
            # skipped, and the tests read the PREVIOUS opcode tables. The
            # mutation then looks survived when nothing was ever built from it.
            #
            # That is machine-speed dependent, so it shows up as an occasional
            # red build on CI and never locally. Deleting them cannot be
            # defeated by any timestamp.
            if path.suffix == ".opcodes":
                for gen in gen_backups:
                    gen.unlink(missing_ok=True)

            built, out = build(args.build_dir, passes=2)
            if not built:
                # A mutation that will not compile proves nothing either way,
                # and is usually a mutation that needs rewriting rather than a
                # problem with the tests. Show enough to fix it.
                first = next((l for l in out.splitlines()
                              if "error" in l.lower()), "")
                print(f"SKIP  {m['name']}\n      does not compile: {first.strip()[:120]}")
                survived.append(m["name"] + " (did not compile)")
                continue

            caught, missed = tests_fail(args.build_dir, m["caught_by"])
            if caught:
                print(f"caught  {m['name']}")
            else:
                print(f"SURVIVED  {m['name']}\n"
                      f"          {missed} still passed with this broken")
                survived.append(m["name"])
        finally:
            shutil.copy2(backup, path)
            # copy2 preserves mtime, leaving the restored file older than the
            # object built from the mutated version. Ninja and make skip the
            # rebuild, so the mutation stays compiled in and every later one
            # runs against a broken emulator and is reported caught.
            os.utime(path, None)
            backup.unlink(missing_ok=True)
            for gen, saved in gen_backups.items():
                shutil.copy2(saved, gen)
                os.utime(gen, None)
                saved.unlink(missing_ok=True)

    # Leave the tree built from clean source.
    ok, _ = build(args.build_dir, passes=2)
    if not ok:
        print("the tree does not build after restoring the sources")
        return 2

    total = len([m for m in MUTATIONS
                 if not args.only or args.only in m["name"]])
    total -= len(not_applicable)
    print(f"\n{total - len(survived)}/{total} mutations caught")
    if not_applicable:
        print(f"{len(not_applicable)} not applicable here (missing fixtures)")
    if survived:
        print("survived:")
        for s in survived:
            print(f"  {s}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
