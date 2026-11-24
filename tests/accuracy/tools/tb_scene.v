// Captures one VGA frame from a gate-level VERA netlist through its package pins.
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

    wire [11:0] rgb = {dut.pin_13, dut.pin_12, dut.pin_11, dut.pin_10,
                       dut.pin_9, dut.pin_6, dut.pin_4, dut.pin_3,
                       dut.pin_2, dut.pin_48, dut.pin_47, dut.pin_46};

    integer fv, fo, fout, rr, reg_i, val_i, i, b0, b1, b2, b3, n, frame, final_ops;
    integer fm, fmo, nm, j, t_i, t_w;
    reg [8*256-1:0] vf, of, outf, mf, mof;
    reg [4:0] op_r [0:65535];
    reg [7:0] op_v [0:65535];
    integer mid_t [0:4095];
    reg [4:0] mid_r [0:4095];
    reg [7:0] mid_v [0:4095];

    // T counts clocks since the latest VSYNC rising edge (pin 45).
    integer ncnt = 0, anchor = 0;
    always @(posedge clk) ncnt = ncnt + 1;
    always @(posedge dut.pin_45) anchor = ncnt;

    // Mid-frame writes: each starts once T reaches its time; its strobe's rising edge is logged.
    task mid_writes;
        begin
            for (j = 0; j < nm; j = j + 1) begin
                while (ncnt - anchor < mid_t[j]) @(negedge clk);
                @(negedge clk); a = mid_r[j]; dout = mid_v[j]; drive = 1;
                #10 cs_n = 0; wr_n = 0;
                repeat (4) @(negedge clk);
                t_w = ncnt - anchor;
                wr_n = 1; cs_n = 1;
                #10 drive = 0;
                $fdisplay(fmo, "%0d", t_w);
            end
        end
    endtask

    initial begin
        if (!$value$plusargs("vram=%s", vf) || !$value$plusargs("ops=%s", of) ||
            !$value$plusargs("out=%s", outf)) $finish;
        if (!$value$plusargs("frame=%d", frame)) frame = 1;
        repeat (3000) @(negedge clk);
        fv = $fopen(vf, "rb");
        for (i = 0; i < 32768; i = i + 1) begin
            b0 = $fgetc(fv); b1 = $fgetc(fv); b2 = $fgetc(fv); b3 = $fgetc(fv);
            if (i < 16384) begin
                dut.spram_3.mem[i] = {b1[7:0], b0[7:0]};
                dut.spram_1.mem[i] = {b3[7:0], b2[7:0]};
            end else begin
                dut.spram_4.mem[i-16384] = {b1[7:0], b0[7:0]};
                dut.spram_2.mem[i-16384] = {b3[7:0], b2[7:0]};
            end
        end

        // The final writes land after VSYNC, so every scene starts on a frame edge.
        if (!$value$plusargs("final=%d", final_ops)) final_ops = 1;
        fo = $fopen(of, "r");
        n = 0;
        rr = $fscanf(fo, "%h %h", reg_i, val_i);
        while (rr == 2) begin
            op_r[n] = reg_i[4:0];
            op_v[n] = val_i[7:0];
            n = n + 1;
            rr = $fscanf(fo, "%h %h", reg_i, val_i);
        end
        nm = 0;
        if ($value$plusargs("mid=%s", mf) && $value$plusargs("midout=%s", mof)) begin
            fm = $fopen(mf, "r");
            rr = $fscanf(fm, "%d %h %h", t_i, reg_i, val_i);
            while (rr == 3) begin
                mid_t[nm] = t_i; mid_r[nm] = reg_i[4:0]; mid_v[nm] = val_i[7:0];
                nm = nm + 1;
                rr = $fscanf(fm, "%d %h %h", t_i, reg_i, val_i);
            end
            fmo = $fopen(mof, "w");
        end
        for (i = 0; i < n - final_ops; i = i + 1) wr(op_r[i], op_v[i]);
        @(posedge dut.pin_45);
        for (i = n - final_ops; i < n; i = i + 1) wr(op_r[i], op_v[i]);
        fork
            if (nm > 0) mid_writes;
            begin
                if (frame > 1) begin
                    for (i = 1; i < frame; i = i + 1) @(posedge dut.pin_45);
                    repeat (26400) @(negedge clk);
                end else begin
                    // wr() spans 13 falling edges.
                    repeat (26400 - 13 * final_ops) @(negedge clk);
                end

                fout = $fopen(outf, "w");
                for (i = 0; i < 384000; i = i + 1) begin
                    @(negedge clk);
                    if (i % 800 < 640) $fdisplay(fout, "%h", rgb);
                end
                $fclose(fout);
            end
        join
        if (nm > 0) $fclose(fmo);
        $finish;
    end
endmodule
