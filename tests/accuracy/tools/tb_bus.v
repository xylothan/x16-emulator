// Replays a bus script on a gate-level VERA netlist and records every read.
//
// T counts clocks since the latest VSYNC rising edge (pin 45). A read is
// stamped at its data sample, a write at its strobe's rising edge, a pin
// sample at the falling clock edge it was taken on.
`timescale 1ns/1ps

module tb;
    parameter SETTLE = 8;
    reg clk = 0, cs_n = 1, rd_n = 1, wr_n = 1, drive = 0;
    reg [4:0] a = 0;
    reg [7:0] dout = 0;
    wire [7:0] d;
    wire [7:0] o;
    assign d = drive ? dout : 8'bz;
    always #20 clk = ~clk;

    chip dut(
        .pin_35(clk), .pin_28(cs_n), .pin_36(rd_n), .pin_31(wr_n),
        .pin_40(a[0]), .pin_39(a[1]), .pin_37(a[2]), .pin_41(a[3]), .pin_34(a[4]),
        .pin_27(d[0]), .pin_26(d[1]), .pin_25(d[2]), .pin_23(d[3]),
        .pin_21(d[4]), .pin_20(d[5]), .pin_19(d[6]), .pin_18(d[7]),
        .pin_17(1'b1),
        .pin_44(o[0]), .pin_42(o[1]), .pin_38(o[2]), .pin_43(o[3]),
        .pin_32(o[4]), .pin_14(o[5]), .pin_15(o[6]), .pin_16(o[7]));

    integer ncnt = 0, anchor = 0;
    always @(posedge clk) ncnt = ncnt + 1;
    always @(posedge dut.pin_45) anchor = ncnt;

    task wr(input [4:0] r, input [7:0] v, output integer t);
        begin
            @(negedge clk); a = r; dout = v; drive = 1;
            #10 cs_n = 0; wr_n = 0;
            repeat (4) @(negedge clk);
            t = ncnt - anchor;
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
            t = ncnt - anchor;
            rd_n = 1; cs_n = 1;
            repeat (SETTLE) @(negedge clk);
        end
    endtask

    integer fi, fo, rr, kind, r, v, t;
    reg [7:0] got;
    reg [8*256-1:0] inf, outf;

    initial begin
        if (!$value$plusargs("in=%s", inf) || !$value$plusargs("out=%s", outf)) $finish;
        repeat (3000) @(negedge clk);
        fi = $fopen(inf, "r");
        fo = $fopen(outf, "w");
        rr = $fscanf(fi, "%d %h %h", kind, r, v);
        while (rr == 3) begin
            case (kind)
                0: begin wr(r[4:0], v[7:0], t); $fdisplay(fo, "w %0d", t); end
                1: begin rd(r[4:0], got, t); $fdisplay(fo, "%h %0d", got, t); end
                2: @(posedge dut.pin_45);
                3: while (ncnt - anchor < v) @(negedge clk);
                4: begin @(negedge clk); $fdisplay(fo, "i %b %0d", o, ncnt - anchor); end
            endcase
            rr = $fscanf(fi, "%d %h %h", kind, r, v);
        end
        $fclose(fo);
        $finish;
    end
endmodule
