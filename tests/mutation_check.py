#!/usr/bin/env python3
"""Check that the tests actually fail when the emulator is wrong.

Break the emulator on purpose, one small change at a time, and check the test
that should care goes red. A mutation that survives means the test is not
covering what it claims, or the code it changed does not matter.

    python tests/mutation_check.py --build-dir build
    python tests/mutation_check.py --build-dir build --changed
    python tests/mutation_check.py --build-dir build --only VIA --only "bus cycle"

Each mutation rebuilds only the executables its caught_by tests run.
Every mutation is reverted afterwards, including on failure or interrupt.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

CMAKE = os.environ.get("CMAKE", "cmake")
CTEST = os.environ.get("CTEST", "ctest")

# Each entry breaks one behaviour and names the tests that should notice.
# `count` is the expected match count; a mismatch is reported, not applied.
MUTATIONS = [
    {
        "name": "PCM FIFO accepts one byte too many",
        "file": "src/vera_pcm.c",
        # audio_fifo.v:27  assign full = (wridx_next == rdidx_r);
        # The ring stores one fewer than its 4096 cells; off by one reads a full FIFO as empty.
        "find": "if (fifo_cnt < sizeof(fifo) - 1) {",
        "into": "if (fifo_cnt <= sizeof(fifo) - 1) {",
        "count": 1,
        "caught_by": ["vera_pcm"],
    },
    {
        "name": "PCM almost-empty boundary is off by one",
        "file": "src/vera_pcm.c",
        # audio_fifo.v:28  assign almost_empty = fifo_count < 12'd1024;
        # Drives the AFLOW interrupt.
        "find": "return fifo_cnt < 1024;",
        "into": "return fifo_cnt <= 1024;",
        "count": 1,
        "caught_by": ["vera_pcm"],
    },
    # No AUDIO_RATE fold mutation: top.v:481 stores raw; recorded as a divergence.
    {
        "name": "VERA debug read clears the FX accumulator",
        "file": "src/video.c",
        "find": "if (!debugOn) fx_mult_accumulator = 0;",
        "into": "fx_mult_accumulator = 0;",
        "count": 1,
        "caught_by": ["vera_debug_read"],
    },
    {
        "name": "VERA DCSEL is truncated to four bits",
        "file": "src/video.c",
        # top.v:89 declares dc_select_r as [5:0] and top.v:337 fills it from
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
        # The non-power-of-two steps let a program walk tile rows.
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
        # R47 form. psg.v:163 (R48) XORs the saw with the inverted pulse width.
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
        "name": "PSG noise LFSR steps once per voice",
        "file": "src/vera_psg.c",
        # VERA 49.0.6 bitstream: one step per sample, shared by every voice.
        "find": "\t\tstruct channel *ch = &channels[i];\n\n\t\t// psg.v:146",
        "into": "\t\tstruct channel *ch = &channels[i];\n\t\tif (i) noise_state = (noise_state << 1) | (((noise_state >> 1) ^ (noise_state >> 2) ^ (noise_state >> 4) ^ (noise_state >> 15)) & 1);\n\n\t\t// psg.v:146",
        "count": 1,
        "caught_by": ["vera_accuracy"],
    },
    {
        "name": "PSG pulse is formed from the updated phase",
        "file": "src/vera_psg.c",
        # psg.v:160 signal_pw compares cur_phase, before this sample's add.
        "find": "case WF_PULSE: v = ((ch->phase >> 10) > ch->pw)",
        "into": "case WF_PULSE: v = ((((ch->phase + ch->freq) & 0x1FFFF) >> 10) > ch->pw)",
        "count": 1,
        "caught_by": ["vera_accuracy"],
    },
    {
        "name": "PSG left sum reaches the DAC a sample early",
        "file": "src/vera_psg.c",
        # The DAC takes the previous sample's sum (psg.v:211-215).
        "find": "*left  = dac_l;",
        "into": "*left  = l;",
        "count": 1,
        "caught_by": ["vera_accuracy"],
    },
    {
        "name": "PCM volume truncates toward zero",
        "file": "src/vera_pcm.c",
        # pcm.v:221 takes bits 21:6 of the product: a floor.
        "find": "*(buf++) = (int16_t)(((int32_t)cur_l * volume_lut[ctrl & 0xF]) >> 6);",
        "into": "*(buf++) = (int16_t)(((int32_t)cur_l * volume_lut[ctrl & 0xF]) / 64);",
        "count": 1,
        "caught_by": ["vera_accuracy"],
    },
    {
        "name": "PCM output survives a FIFO reset",
        "file": "src/vera_pcm.c",
        # pcm.v:104-108 zeroes the output while the FIFO is empty.
        "find": "zero at once.\n\tif (fifo_cnt == 0) {",
        "into": "zero at once.\n\tif (0) {",
        "count": 1,
        "caught_by": ["vera_accuracy"],
    },
    {
        "name": "PCM holds its output after a partial sample",
        "file": "src/vera_pcm.c",
        # pcm.v:104-108: bytes taken short of a sample leave the output at zero.
        "find": "forces the output to zero.\n\t\tif (fifo_cnt == 0) {",
        "into": "forces the output to zero.\n\t\tif (0) {",
        "count": 1,
        "caught_by": ["vera_accuracy"],
    },
    {
        "name": "VERA ISR clears on a written zero",
        "file": "src/video.c",
        # top.v:441-443 clear a latch when the written bit is one:
        #   irq_status_vsync_next = irq_status_vsync_r & !write_data[0];
        # Inverting that acknowledges every pending source whenever a program clears one.
        "find": "isr &= ~(value & 0x07);",
        "into": "isr &= ~(~value & 0x07);",
        "count": 1,
        "caught_by": ["vera_irq"],
    },
    {
        "name": "VERA IEN stores a fifth enable bit",
        "file": "src/video.c",
        # top.v:178 reads bits 5:4 as 2'b0 and top.v:434-437 give them no write
        # target, so a program cannot store anything there.
        "find": "ien = value & 0xF;",
        "into": "ien = value & 0x1F;",
        "count": 1,
        "caught_by": ["vera_irq"],
    },
    {
        "name": "VERA IRQLINE bit 8 comes from the wrong IEN bit",
        "file": "src/video.c",
        # top.v:433  irq_line_next[8] = write_data[7];
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
        # top.v:179 takes bit 3 from the audio_fifo_low wire, not a latch, so
        # it reports the FIFO level at the instant of the read.
        "find": "case 0x07: return isr | (pcm_is_fifo_almost_empty() ? 8 : 0);",
        "into": "case 0x07: return isr;",
        "count": 1,
        "caught_by": ["vera_irq"],
    },
    {
        "name": "VERA asserts IRQ regardless of the enables",
        "file": "src/video.c",
        # top.v:1201  assign extbus_irq_n = (irq_status & irq_enable) == 0;
        # Dropping the mask interrupts on every VSYNC.
        "find": "return (tmp_isr & ien) != 0;",
        "into": "return tmp_isr != 0;",
        "count": 1,
        "caught_by": ["vera_irq"],
    },
    {
        "name": "VERA IEN bit 6 reports a stored bit, not the scanline",
        "file": "src/video.c",
        # top.v:178 sources bit 6 from scanline[8], live, with nothing writing it.
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
        "find": "sprite_line_collisions |= sprite_line_mask[line_x] & props->sprite_collision_mask;",
        "into": "sprite_line_collisions |= sprite_line_mask[line_x] & ~props->sprite_collision_mask;",
        "count": 1,
        "caught_by": ["vera_irq"],
    },
    {
        "name": "VERA SCANLINE reads the beam line",
        "file": "src/video.c",
        # composer.v:126-127: SCANLINE is the composer's counter, a line ahead.
        "find": "return prev >= 512 ? 511 : comp_line_counter(y) & 0x1FF;",
        "into": "return prev >= 512 ? 511 : y & 0x1FF;",
        "count": 1,
        "caught_by": ["vera_accuracy"],
    },
    {
        "name": "VERA line IRQ status sets on the line's first clock",
        "file": "src/video.c",
        # top.v:451-452: line_irq is registered before the status latch.
        "find": "if (line_irq_pending && vga_scan_pos_x >= 2) {",
        "into": "if (line_irq_pending) {",
        "count": 1,
        "caught_by": ["vera_accuracy"],
    },
    {
        "name": "VERA hidden sprite pixel writes its collision mask",
        "file": "src/video.c",
        # sprite_renderer.v:319: the mask goes in with the pixel, if it is drawn.
        "find": "sprite_line_collisions |= sprite_line_mask[line_x] & props->sprite_collision_mask;",
        "into": "sprite_line_collisions |= sprite_line_mask[line_x] & props->sprite_collision_mask;"
                " sprite_line_mask[line_x] |= props->sprite_collision_mask;",
        "count": 1,
        "caught_by": ["vera_accuracy"],
    },
    {
        "name": "VERA sprite fetch ignores layer requests",
        "file": "src/video.c",
        # vram_if.v:124-138: sprites are granted VRAM last.
        "find": "while (rt < SPRITE_RENDER_TIME && taken[rt]) {",
        "into": "while (rt < SPRITE_RENDER_TIME && taken[rt] && rt < 1) {",
        "count": 1,
        "caught_by": ["vera_accuracy"],
    },
    {
        "name": "VERA sprite fetch ack is combinational",
        "file": "src/video.c",
        # vram_if.v:142-146: the ack is registered.
        "find": "\t\t\t\trt += 2;\n",
        "into": "\t\t\t\trt += 1;\n",
        "count": 1,
        "caught_by": ["vera_accuracy"],
    },
    {
        "name": "VERA sprite pixel on the last render clock is drawn",
        "file": "src/video.c",
        "find": "const bool last = rt == SPRITE_RENDER_TIME;",
        "into": "const bool last = rt == SPRITE_RENDER_TIME + 1;",
        "count": 1,
        "caught_by": ["vera_accuracy"],
    },
    {
        "name": "VERA sprite pixel on the last render clock is dropped",
        "file": "src/video.c",
        # top.v:731-739: it is written into the buffer rendered next.
        "find": "sprite_carry = (sprite_carry_t){ 1,",
        "into": "sprite_carry = (sprite_carry_t){ 0,",
        "count": 1,
        "caught_by": ["vera_accuracy"],
    },
    {
        "name": "VERA sprites drawn only on lines 0-479",
        "file": "src/video.c",
        # composer.v:147-162: render_start follows the window, not the display.
        "find": "\tif (sprite_start) {\n",
        "into": "\tif (sprite_start && y < SCREEN_HEIGHT) {\n",
        "count": 1,
        "caught_by": ["vera_accuracy"],
    },
    {
        "name": "VERA palette write lands a pixel early",
        "file": "src/video.c",
        "find": "scan_out_to(write_dot());",
        "into": "scan_out_to(write_dot() - 1);",
        "count": 1,
        "caught_by": ["vera_accuracy"],
    },
    {
        "name": "VERA layer 1 wins VRAM arbitration",
        "file": "src/video.c",
        # vram_if.v:133-158: interface 1 (layer 0) before interface 2 (layer 1).
        "find": "s[1].ack = req1 && !req0;",
        "into": "s[1].ack = req1;",
        "count": 1,
        "caught_by": ["vera_accuracy"],
    },
    {
        "name": "VERA layer fetch ignores writes made during the line",
        "file": "src/video.c",
        "find": "layers_written |= e->reg <= FL_DC_VIDEO;",
        "into": "layers_written |= false;",
        "count": 1,
        "caught_by": ["vera_accuracy"],
    },
    {
        "name": "VERA ISR write clears the collision bits",
        "file": "src/video.c",
        "find": "isr &= ~(value & 0x07);",
        "into": "isr &= ~value;",
        "count": 1,
        "caught_by": ["vera_irq"],
    },
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
        # top.v:384-385 forces tile_baseaddr[1:0] to zero on write; bits 1 and 0
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
        # top.v:222-228 gives layer 1 its own registers at 5'h14-5'h1A. One
        # bank behind both sets of addresses would leave the two layers sharing
        # a configuration.
        "find": "reg_layer[1][reg - 0x14] = (reg == 0x18 || reg == 0x1A) ? (value & 0x0f) : value;",
        "into": "reg_layer[0][reg - 0x14] = (reg == 0x18 || reg == 0x1A) ? (value & 0x0f) : value;",
        "count": 1,
        "caught_by": ["vera_layer"],
    },
    {
        "name": "VERA layer 0 scroll high bytes keep bits 7:4",
        "file": "src/video.c",
        "find": "reg_layer[0][reg - 0x0D] = (reg == 0x11 || reg == 0x13) ? (value & 0x0f) : value;",
        "into": "reg_layer[0][reg - 0x0D] = value;",
        "count": 1,
        "caught_by": ["vera_layer"],
    },
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
        # top.v:387-388 takes height from write_data[1] and width from
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
        # palette_ram.mem is the bitstream's default palette; compared against the file.
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
    # YM2151 interface. Expected values: Nuked-OPM, tests/accuracy/ym2151.
    {
        "name": "YM2151 busy rises with the strobe",
        "file": "src/ymglue.cpp",
        "find": "busy = (!(busy_cnt >> 5) && busy) || wd_en;",
        "into": "busy = (!(busy_cnt >> 5) && busy) || wd_en || wd;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 busy lasts 16 cycles",
        "file": "src/ymglue.cpp",
        "find": "busy = (!(busy_cnt >> 5) && busy) || wd_en;",
        "into": "busy = (!(busy_cnt >> 4) && busy) || wd_en;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 refuses writes while busy",
        "file": "src/ymglue.cpp",
        "find": "\t\tconst bool clobbers = wa || wd || wa_en || wd_en;\n",
        "into": "\t\tif (busy) return false;\n\t\tconst bool clobbers = wa || wd || wa_en || wd_en;\n",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 a write reaches the chip a cycle early",
        "file": "src/ymglue.cpp",
        "find": "\t\t\twa = true;\n",
        "into": "\t\t\twa_en = true;\n",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 data waits for no slot",
        "file": "src/ymglue.cpp",
        "find": "if (data_ok && !data_done && scans(addr, cycle)) {",
        "into": "if (data_ok && !data_done) {",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 channel registers wait for a 32-cycle slot",
        "file": "src/ymglue.cpp",
        "find": "return a < 0x40 ? (a & 7) == (cycle & 7)",
        "into": "return a < 0x40 ? (a & 7) == cycle",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 an address write keeps pending data",
        "file": "src/ymglue.cpp",
        "find": "data_ok = data_ok && !wa_en;",
        "into": "data_ok = data_ok && true;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 timer registers wait for the slot scan",
        "file": "src/ymglue.cpp",
        "find": "if (wd_en && mode_addr < 0x20) {",
        "into": "if (wd_en && mode_addr < 0x10) {",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 timer A counts twice a sample",
        "file": "src/ymglue.cpp",
        "find": "a_inc = a_load && cycle == 0;",
        "into": "a_inc = a_load && (cycle & 15) == 0;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 timer B prescaler divides by 8",
        "file": "src/ymglue.cpp",
        "find": "if (cycle == 0) b_sub++;\n\t\tb_sub_of = (b_sub >> 4) & 1;",
        "into": "if (cycle == 0) b_sub++;\n\t\tb_sub_of = (b_sub >> 3) & 1;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 timer B prescaler stops while idle",
        "file": "src/ymglue.cpp",
        "find": "\t\t\t\tb_sub++;\n",
        "into": "",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 LOAD is taken on any cycle",
        "file": "src/ymglue.cpp",
        "find": "if (cycle == 1) a_load = a_load_reg;",
        "into": "a_load = a_load_reg;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 timer flag set without IRQEN",
        "file": "src/ymglue.cpp",
        "find": "a_flag = !a_reset && (a_flag || (a_irqen && a_of));",
        "into": "a_flag = !a_reset && (a_flag || a_of);",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 RESET leaves timer B's flag",
        "file": "src/ymglue.cpp",
        "find": "b_flag = !b_reset && (",
        "into": "b_flag = (",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 a settled sample is skipped whole",
        "file": "src/ymglue.cpp",
        "find": "if (cycle >= 1 && settled()) {",
        "into": "if (settled()) {",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 IRQ follows the flags in the same cycle",
        "file": "src/ymglue.cpp",
        "find": "\t\tcycle = (cycle + 1) & 31;\n\t}",
        "into": "\t\tirq = a_flag || b_flag;\n\t\tcycle = (cycle + 1) & 31;\n\t}",
        "count": 1,
        "caught_by": ["ym_accuracy"],
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
]


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, **kw)


def test_targets(build_dir, config):
    """Map each ctest name to the executable target it runs, from ctest itself.

    A test whose command runs no <name>_test executable maps to None.
    """
    r = run([CTEST, "--test-dir", build_dir, "-C", config, "--show-only=json-v1"])
    if r.returncode != 0:
        return {}
    try:
        tests = json.loads(r.stdout).get("tests", [])
    except ValueError:
        return {}
    out = {}
    for t in tests:
        target = None
        for arg in t.get("command", []):
            stem = Path(arg).stem
            if stem.endswith("_test"):
                target = stem
        out[t["name"]] = target
    return out


def targets_for(names, known):
    """The targets that build the named tests; the whole suite if any is unknown."""
    found = [known.get(n) for n in names]
    if not found or None in found:
        return ["unit_tests"]
    return sorted(set(found))


def changed_files():
    """Files that differ from origin/main, committed or not."""
    out = set()
    for cmd in (["git", "diff", "--name-only", "origin/main...HEAD"],
                ["git", "diff", "--name-only", "HEAD"]):
        r = run(cmd)
        out |= {l.strip() for l in r.stdout.splitlines() if l.strip()}
    return out


def build(build_dir, config, passes=1, targets=("unit_tests",)):
    """Build the given targets, optionally more than once.

    An .opcodes mutation regenerates src/cpu/tables.h during the build, and the
    objects that include it are only recompiled on the following pass.
    """
    out = ""
    for _ in range(passes):
        r = run([CMAKE, "--build", build_dir, "--config", config, "--target", *targets])
        out += r.stdout + r.stderr
        if r.returncode != 0:
            return False, out
    return True, out


def test_result(build_dir, config, name):
    """'passed', 'failed' or 'not run', from ctest's JUnit report.

    ctest's exit code is no use alone: a test it cannot find, or one with no
    executable for this configuration, exits non-zero as "Not Run".
    """
    fd, report = tempfile.mkstemp(suffix=".xml")
    os.close(fd)
    try:
        run([CTEST, "--test-dir", build_dir, "-C", config, "-R", f"^{name}$",
             "--output-junit", report])
        try:
            cases = [c for c in ET.parse(report).getroot().iter("testcase")
                     if c.get("name") == name]
        except (ET.ParseError, OSError):
            cases = []
    finally:
        os.unlink(report)
    if len(cases) != 1:
        return "not run"
    return {"run": "passed", "fail": "failed"}.get(cases[0].get("status"), "not run")


def tests_fail(build_dir, config, names):
    """(caught, name): caught when every named test fails; else the first that did not."""
    for name in names:
        result = test_result(build_dir, config, name)
        if result != "failed":
            return False, f"{name} {result}"
    return True, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("--config", default="Debug",
                    help="build configuration, for multi-config generators")
    ap.add_argument("--only", action="append", default=[],
                    help="run the mutations whose name contains this; repeatable")
    ap.add_argument("--changed", action="store_true",
                    help="run the mutations whose file differs from origin/main")
    ap.add_argument("--list", action="store_true",
                    help="print the selected mutations and their targets, then stop")
    args = ap.parse_args()

    changed = changed_files() if args.changed else None
    selected = [m for m in MUTATIONS
                if (not args.only or any(o in m["name"] for o in args.only))
                and (changed is None or m["file"] in changed)]

    known = test_targets(args.build_dir, args.config)
    plan = [(m, targets_for(m["caught_by"], known)) for m in selected]
    if args.list:
        for m, t in plan:
            print(f"{m['name']}\n      {m['file']} -> {' '.join(t)}")
        print(f"\n{len(plan)} mutations")
        return 0

    every = sorted({t for _, ts in plan for t in ts}) or ["unit_tests"]
    if "unit_tests" in every:
        every = ["unit_tests"]
    ok, out = build(args.build_dir, args.config, targets=every)
    if not ok:
        print("the tree does not build before any mutation:\n" + out[-2000:])
        return 2

    # A test that fails, or does not run, on clean source catches every mutation.
    baseline = sorted({n for m, _ in plan
                       if not (m.get("requires") and not (ROOT / m["requires"]).exists())
                       for n in m["caught_by"]})
    broken = [(n, r) for n in baseline
              if (r := test_result(args.build_dir, args.config, n)) != "passed"]
    if broken:
        print("these tests do not pass before any mutation:")
        for n, r in broken:
            print(f"  {n}: {r}")
        return 2

    survived = []
    not_applicable = []
    for m, targets in plan:

        # Without its fixture the test skips and looks like the mutation survived.
        need = m.get("requires")
        if need and not (ROOT / need).exists():
            print(f"n/a   {m['name']}\n      needs {need}, which is not present")
            not_applicable.append(m["name"])
            continue

        path = ROOT / m["file"]
        # Universal newlines: \n patterns match a CRLF checkout; bytes are restored after.
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

            # Delete generated headers: a restore inside one mtime tick skips regeneration.
            if path.suffix == ".opcodes":
                for gen in gen_backups:
                    gen.unlink(missing_ok=True)

            built, out = build(args.build_dir, args.config, passes=2, targets=targets)
            if not built:
                # A mutation that will not compile proves nothing; show enough to fix it.
                first = next((l for l in out.splitlines()
                              if "error" in l.lower()), "")
                print(f"SKIP  {m['name']}\n      does not compile: {first.strip()[:120]}")
                survived.append(m["name"] + " (did not compile)")
                continue

            caught, missed = tests_fail(args.build_dir, args.config, m["caught_by"])
            if caught:
                print(f"caught  {m['name']}")
            else:
                print(f"SURVIVED  {m['name']}\n"
                      f"          {missed} with this broken")
                survived.append(m["name"])
        finally:
            shutil.copy2(backup, path)
            # Restore mtime after copy2, or the build skips it and the mutation stays in.
            os.utime(path, None)
            backup.unlink(missing_ok=True)
            for gen, saved in gen_backups.items():
                shutil.copy2(saved, gen)
                os.utime(gen, None)
                saved.unlink(missing_ok=True)

    # Leave the tree built from clean source.
    ok, _ = build(args.build_dir, args.config, passes=2, targets=every)
    if not ok:
        print("the tree does not build after restoring the sources")
        return 2

    total = len(plan) - len(not_applicable)
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
