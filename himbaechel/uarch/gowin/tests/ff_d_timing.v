// Fixed-placement STA fixture: one register-to-register SEL path. No PLL,
// generated-clock or external I/O timing claim is made by this fixture.
module top(input clk, data, a, b, output q, lut_out);
    wire source_q;
    (* keep, BEL="X11Y1/DFF0" *) DFF source(.CLK(clk), .D(data), .Q(source_q));
    (* keep, BEL="X10Y1/DFF0" *) DFF sink(.CLK(clk), .D(source_q), .Q(q));
    (* keep, BEL="X10Y1/LUT0" *)
    LUT4 #(.INIT(16'h8888)) unrelated(.I0(a), .I1(b), .I2(1'b0), .I3(1'b0), .F(lut_out));
endmodule
