"""Mutations for VERA video, PSG and PCM."""

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
]
