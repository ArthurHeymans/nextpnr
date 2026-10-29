module top(input clk, a, b, sel, data, output lut_out, q);
    (* keep, BEL="X10Y1/LUT0" *)
    LUT4 #(.INIT(16'h8888)) logic_lut(.I0(a), .I1(b), .I2(1'b0), .I3(1'b0), .F(lut_out));
`ifdef WIDE
    wire other_out, wide_out;
    (* keep, BEL="X10Y1/LUT1" *)
    LUT4 #(.INIT(16'heeee)) other_lut(.I0(a), .I1(b), .I2(1'b0), .I3(1'b0), .F(other_out));
    (* keep, BEL="X10Y1/MUX0" *)
    MUX2_LUT5 wide(.I0(lut_out), .I1(other_out), .S0(sel), .O(wide_out));
    // Keep the wide mux observable without changing the register's D.
    wire used = wide_out;
`else
    wire used = sel;
`endif
`ifdef PAIRED
    wire ff_data = lut_out;
`elsif CONSTANT
    wire ff_data = 1'b0;
`elsif SHARED_SELECT
    wire ff_data = sel;
`else
    wire ff_data = data;
`endif
    wire ff_q;
    (* keep, BEL="X10Y1/DFF0" *) DFF reg_d(.CLK(clk), .D(ff_data), .Q(ff_q));
    assign q = ff_q ^ used;
endmodule
