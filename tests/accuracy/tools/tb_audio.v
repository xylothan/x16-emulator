// Replays an audio script on a gate-level VERA netlist and records the I2S output.
//
// T counts clocks from the start of simulation. A write is stamped at its
// strobe's rising edge, a read at its data sample. Each I2S half-frame is
// printed when LRCK (pin 43) changes: the clock of that change, the new LRCK
// level and the 32 bits shifted in on BCK (pin 38) rising edges before it,
// from DATA (pin 42).
`timescale 1ns/1ps

module tb;
    parameter SETTLE = 8;
    reg clk = 0, cs_n = 1, rd_n = 1, wr_n = 1, drive = 0;
    reg [4:0] a = 0;
    reg [7:0] dout = 0;
    wire [7:0] d;
    wire lrck, bck, data;
    assign d = drive ? dout : 8'bz;
    always #20 clk = ~clk;

    chip dut(
        .pin_35(clk), .pin_28(cs_n), .pin_36(rd_n), .pin_31(wr_n),
        .pin_40(a[0]), .pin_39(a[1]), .pin_37(a[2]), .pin_41(a[3]), .pin_34(a[4]),
        .pin_27(d[0]), .pin_26(d[1]), .pin_25(d[2]), .pin_23(d[3]),
        .pin_21(d[4]), .pin_20(d[5]), .pin_19(d[6]), .pin_18(d[7]),
        .pin_17(1'b1),
        .pin_43(lrck), .pin_38(bck), .pin_42(data));

    integer ncnt = 0;
    always @(posedge clk) ncnt = ncnt + 1;

    integer fo;
    reg capturing = 0, lr_q = 0, bck_q = 0;
    reg [31:0] word = 0;
    always @(negedge clk) begin
        if (capturing) begin
            if (bck && !bck_q) word = {word[30:0], data};
            if (lrck !== lr_q) begin
                $fdisplay(fo, "F %0d %0d %h", ncnt, lrck, word);
                word = 0;
            end
        end
        lr_q = lrck;
        bck_q = bck;
    end

    task wr(input [4:0] r, input [7:0] v, output integer t);
        begin
            @(negedge clk); a = r; dout = v; drive = 1;
            #10 cs_n = 0; wr_n = 0;
            repeat (4) @(negedge clk);
            t = ncnt;
            wr_n = 1; cs_n = 1;
            #10 drive = 0;
            repeat (SETTLE) @(negedge clk);
        end
    endtask

    task rd(input [4:0] r, output [7:0] v, output integer t);
        begin
            @(negedge clk); a = r;
            #10 cs_n = 0; rd_n = 0;
            repeat (4) @(negedge clk);
            v = d;
            t = ncnt;
            rd_n = 1; cs_n = 1;
            repeat (SETTLE) @(negedge clk);
        end
    endtask

    integer fi, rr, kind, r, v, t;
    reg [7:0] got;
    reg [8*256-1:0] inf, outf;

    initial begin
        if (!$value$plusargs("in=%s", inf) || !$value$plusargs("out=%s", outf)) $finish;
        fi = $fopen(inf, "r");
        fo = $fopen(outf, "w");
        capturing = 1;
        rr = $fscanf(fi, "%d %h %h", kind, r, v);
        while (rr == 3) begin
            case (kind)
                0: begin wr(r[4:0], v[7:0], t); $fdisplay(fo, "w %0d", t); end
                1: begin rd(r[4:0], got, t); $fdisplay(fo, "r %h %0d", got, t); end
                3: while (ncnt < v) @(negedge clk);
            endcase
            rr = $fscanf(fi, "%d %h %h", kind, r, v);
        end
        $fclose(fo);
        $finish;
    end
endmodule
