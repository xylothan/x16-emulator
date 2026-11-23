// Replays a bus script on a gate-level VERA netlist and records every read.
`timescale 1ns/1ps

module tb;
    parameter SETTLE = 8;
    reg clk = 0, cs_n = 1, rd_n = 1, wr_n = 1, drive = 0;
    reg [4:0] a = 0;
    reg [7:0] dout = 0;
    wire [7:0] d;
    assign d = drive ? dout : 8'bz;
    always #20 clk = ~clk;

    chip dut(
        .pin_35(clk), .pin_28(cs_n), .pin_36(rd_n), .pin_31(wr_n),
        .pin_40(a[0]), .pin_39(a[1]), .pin_37(a[2]), .pin_41(a[3]), .pin_34(a[4]),
        .pin_27(d[0]), .pin_26(d[1]), .pin_25(d[2]), .pin_23(d[3]),
        .pin_21(d[4]), .pin_20(d[5]), .pin_19(d[6]), .pin_18(d[7]),
        .pin_17(1'b1));

    task wr(input [4:0] r, input [7:0] v);
        begin
            @(negedge clk); a = r; dout = v; drive = 1;
            #10 cs_n = 0; wr_n = 0;
            repeat (4) @(negedge clk);
            wr_n = 1; cs_n = 1;
            #10 drive = 0;
            repeat (SETTLE) @(negedge clk);
        end
    endtask

    task rd(input [4:0] r, output [7:0] v);
        begin
            @(negedge clk); a = r;
            #10 cs_n = 0; rd_n = 0;
            repeat (4) @(negedge clk);
            v = d;
            rd_n = 1; cs_n = 1;
            repeat (SETTLE) @(negedge clk);
        end
    endtask

    integer fi, fo, rr, kind, r, v;
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
                0: wr(r[4:0], v[7:0]);
                1: begin rd(r[4:0], got); $fdisplay(fo, "%h", got); end
                2: @(posedge dut.pin_45);
            endcase
            rr = $fscanf(fi, "%d %h %h", kind, r, v);
        end
        $fclose(fo);
        $finish;
    end
endmodule
