`timescale 1ns/1ps

// The part of top.v that surrounds addr_data: CTRL decode and the read mux
// (top.v:166-173, 179-209). Version bytes are compared by neither side.
module fx_wrap(
    input  wire       clk,
    input  wire       reset,
    input  wire       do_read,
    input  wire       do_write,
    input  wire [4:0] access_addr,
    input  wire [7:0] write_data,
    output reg  [7:0] rddata);

    reg        sel_r;
    reg  [5:0] dc_r;

    always @(posedge clk or posedge reset)
        if (reset) begin
            sel_r <= 0;
            dc_r  <= 0;
        end else if (do_write && access_addr == 5'h05) begin
            dc_r  <= write_data[6:1];
            sel_r <= write_data[0];
        end

    wire [16:0] a0, a1, ib_addr;
    wire        n0, n1, ni0, ni1, d0f, d1f;
    wire  [3:0] i0, i1;
    wire  [7:0] data0, data1, vram_rddata, ib_cache8, ib_wrdata, fill_l, fill_h;
    wire [31:0] ib_cache32;
    wire        ib_nib, ib_4, ib_cw, ib_tr, ib_cyc, ib_write, ib_do;
    wire        fx_tr, fx_cw, fx_cf, fx_cyc, fx_hop, fx_4;
    wire  [1:0] fx_mode;

    addr_data ad(
        .reset(reset), .clk(clk), .do_read(do_read), .do_write(do_write),
        .access_addr(access_addr), .write_data(write_data), .vram_rddata(vram_rddata),
        .vram_addr_select(sel_r), .dc_select(dc_r),
        .vram_addr_0(a0), .vram_addr_1(a1), .vram_addr_nib_0(n0), .vram_addr_nib_1(n1),
        .vram_addr_incr_0(i0), .vram_addr_incr_1(i1),
        .vram_addr_nib_incr_0(ni0), .vram_addr_nib_incr_1(ni1),
        .vram_addr_decr_0(d0f), .vram_addr_decr_1(d1f),
        .vram_data0(data0), .vram_data1(data1),
        .ib_addr(ib_addr), .ib_addr_nibble(ib_nib), .ib_4bit_mode(ib_4),
        .ib_cache_write_enabled(ib_cw), .ib_transparency_enabled(ib_tr),
        .ib_one_byte_cache_cycling(ib_cyc), .ib_mult_accum_cache32(ib_cache32),
        .ib_cache8(ib_cache8), .ib_wrdata(ib_wrdata), .ib_write(ib_write), .ib_do_access(ib_do),
        .fx_transparency_enabled(fx_tr), .fx_cache_write_enabled(fx_cw),
        .fx_cache_fill_enabled(fx_cf), .fx_one_byte_cache_cycling(fx_cyc),
        .fx_16bit_hop(fx_hop), .fx_4bit_mode(fx_4), .fx_addr1_mode(fx_mode),
        .fx_fill_length_low(fill_l), .fx_fill_length_high(fill_h));

    vram_if vram(
        .clk(clk),
        .if0_addr(ib_addr), .if0_addr_nibble(ib_nib), .if0_4bit_mode(ib_4),
        .if0_cache_write_enabled(ib_cw), .if0_transparency_enabled(ib_tr),
        .if0_one_byte_cache_cycling(ib_cyc), .if0_cache8(ib_cache8),
        .if0_mult_accum_cache32(ib_cache32), .if0_wrdata(ib_wrdata),
        .if0_rddata(vram_rddata), .if0_strobe(ib_do), .if0_write(ib_write),
        .if1_addr(15'd0), .if1_strobe(1'b0),
        .if2_addr(15'd0), .if2_strobe(1'b0),
        .if3_addr(15'd0), .if3_strobe(1'b0));

    always @* begin
        rddata = 8'h00;
        case (access_addr)
            5'h00: rddata = sel_r ? a1[7:0]  : a0[7:0];
            5'h01: rddata = sel_r ? a1[15:8] : a0[15:8];
            5'h02: rddata = sel_r ? {i1, d1f, ni1, n1, a1[16]} : {i0, d0f, ni0, n0, a0[16]};
            5'h03: rddata = data0;
            5'h04: rddata = data1;
            5'h05: rddata = {1'b0, dc_r, sel_r};
            5'h09: if (dc_r == 2) rddata = {fx_tr, fx_cw, fx_cf, fx_cyc, fx_hop, fx_4, fx_mode};
            5'h0B: if (dc_r == 5) rddata = fill_l;
            5'h0C: if (dc_r == 5) rddata = fill_h;
            default: ;
        endcase
    end
endmodule

module tb;
    parameter SETTLE = 16;

    reg clk = 0, reset = 1, do_read = 0, do_write = 0;
    reg [4:0] access_addr = 0;
    reg [7:0] write_data = 0;
    wire [7:0] rddata;

    always #20 clk = ~clk;

    fx_wrap dut(.clk(clk), .reset(reset), .do_read(do_read), .do_write(do_write),
                .access_addr(access_addr), .write_data(write_data), .rddata(rddata));

    integer fin, fout, r, kind, reg_i, val_i, i;
    reg [8*256-1:0] opsf, outf;
    reg [7:0]  rd;
    reg [16:0] pre;
    reg [31:0] word;
    reg [16:0] ba;

    function [7:0] pattern(input [16:0] a);
        reg [31:0] x;
        begin
            x = a;
            pattern = ((x * 37) ^ (x >> 5) ^ (x >> 11)) & 8'hff;
        end
    endfunction

    task reinit;
        begin
            reset = 1;
            do_read = 0; do_write = 0;
            repeat (4) @(negedge clk);
            dut.ad.mult_accum.q = 0;
            for (i = 0; i < 32768; i = i + 1) begin
                ba = i << 2;
                dut.vram.main_ram.mem[i] = {pattern(ba + 3), pattern(ba + 2), pattern(ba + 1), pattern(ba)};
            end
            reset = 0;
            repeat (4) @(negedge clk);
        end
    endtask

    initial begin
        if (!$value$plusargs("ops=%s", opsf) || !$value$plusargs("out=%s", outf)) begin
            $display("need +ops= +out=");
            $finish;
        end
        fin  = $fopen(opsf, "r");
        fout = $fopen(outf, "w");
        reinit;
        r = $fscanf(fin, "%d %h %h\n", kind, reg_i, val_i);
        while (r == 3) begin
            if (kind == 2) begin
                reinit;
                $fdisplay(fout, "S");
            end else begin
                @(negedge clk);
                access_addr = reg_i[4:0];
                write_data  = val_i[7:0];
                pre = (reg_i == 3) ? dut.a0 : dut.a1;
                #1 rd = (kind == 0) ? rddata : 8'h00;
                @(negedge clk);
                do_write = (kind == 1);
                do_read  = (kind == 0);
                @(negedge clk);
                do_write = 0;
                do_read  = 0;
                repeat (SETTLE) @(negedge clk);
                word = 0;
                if (kind == 1 && (reg_i == 3 || reg_i == 4))
                    word = dut.vram.main_ram.mem[pre[16:2]];
                $fdisplay(fout, "%h %h %0d %h %0d %h %h %h %h %h %h %0d %h",
                    rd, dut.a0, dut.n0, dut.a1, dut.n1, dut.data0, dut.data1,
                    dut.ad.fx_pixel_pos_x_r, dut.ad.fx_pixel_pos_y_r,
                    dut.ad.ib_cache32_r, dut.ad.mult_accum.q,
                    dut.ad.fx_2bit_poke_mode_r, word);
            end
            r = $fscanf(fin, "%d %h %h\n", kind, reg_i, val_i);
        end
        $fclose(fout);
        $finish;
    end
endmodule
