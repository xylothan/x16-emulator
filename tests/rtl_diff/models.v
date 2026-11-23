// Simulation models for the two hard cells behind addr_data.v / vram_if.v.

// pmi_dsp as configured by fpga/source/mult_accum.v, with the semantics of
// SB_MAC16 in yosys techlibs/ice40/cells_sim.v:
//   P = OLOAD ? {C,D} : Q +/- (A*B);  O = P;  Q <= P unless OHOLD;  ORST clears Q.
module mult_accum(
    input  wire        clk,
    input  wire [15:0] input_a_16,
    input  wire [15:0] input_b_16,
    input  wire        mult_enabled,
    input  wire        reset_accum,
    input  wire        accumulate,
    input  wire        add_or_sub,
    output wire [31:0] output_32);

    reg  [31:0] q = 0;
    wire signed [15:0] a = input_a_16;
    wire signed [15:0] b = input_b_16;
    wire signed [31:0] prod = a * b;
    wire [31:0] sum = add_or_sub ? (q - prod) : (q + prod);
    assign output_32 = mult_enabled ? sum : {input_b_16, input_a_16};

    always @(posedge clk or posedge reset_accum)
        if (reset_accum)     q <= 0;
        else if (accumulate) q <= output_32;
endmodule

// main_ram.v's __ICARUS__ branch indexes its 16k-word arrays with the full
// 15-bit address on write, which drops writes above $10000. This models the
// SP256K path: 32k x 32, nibble write mask, registered read.
module main_ram(
    input  wire        clk,
    input  wire [14:0] bus_addr,
    input  wire [31:0] bus_wrdata,
    input  wire  [7:0] bus_wrnibblesel,
    output reg  [31:0] bus_rddata,
    input  wire        bus_write);

    reg [31:0] mem [0:32767];
    reg [31:0] w;
    integer n;

    always @(posedge clk) begin
        if (bus_write) begin
            w = mem[bus_addr];
            for (n = 0; n < 8; n = n + 1)
                if (bus_wrnibblesel[n]) w[n*4 +: 4] = bus_wrdata[n*4 +: 4];
            mem[bus_addr] = w;
        end
        bus_rddata <= mem[bus_addr];
    end
endmodule
