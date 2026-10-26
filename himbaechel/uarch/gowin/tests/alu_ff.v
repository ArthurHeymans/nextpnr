module top(input clk, input ce0, ce1, input [5:0] a, b, output [5:0] q);
    wire [6:0] carry;
    wire [5:0] sum;
    assign carry[0] = 1'b0;
    for (genvar i = 0; i < 6; i = i + 1) begin
        ALU #(.ALU_MODE(2)) alu (
            .I0(a[i]), .I1(b[i]), .I3(1'b0), .CIN(carry[i]), .COUT(carry[i+1]), .SUM(sum[i])
        );
        // One incompatible control must not make the carry cluster unplaceable.
        DFFRE ff (.CLK(clk), .CE(i == 1 ? ce1 : ce0), .RESET(1'b0), .D(sum[i]), .Q(q[i]));
    end
endmodule
