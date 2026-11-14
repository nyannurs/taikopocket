// Clock-domain crossing helpers for the Taiko gateware (GPL-3.0, Taiko Pocket).

// A single-cycle pulse from one clock domain to another (toggle + 3 flops).
module taiko_cdc_pulse (
    input  wire clk_a,
    input  wire pulse_a,
    input  wire clk_b,
    output wire pulse_b
);
  reg toggle_a = 0;
  always @(posedge clk_a) if (pulse_a) toggle_a <= ~toggle_a;

  reg [2:0] sync_b = 0;
  always @(posedge clk_b) sync_b <= {sync_b[1:0], toggle_a};
  assign pulse_b = sync_b[2] ^ sync_b[1];
endmodule

// A slowly changing bit into another clock domain.
module taiko_cdc_bit #(
    parameter WIDTH = 1
) (
    input  wire             clk,
    input  wire [WIDTH-1:0] d,
    output wire [WIDTH-1:0] q
);
  reg [WIDTH-1:0] s0 = 0, s1 = 0;
  always @(posedge clk) begin
    s0 <= d;
    s1 <= s0;
  end
  assign q = s1;
endmodule

// A multi-bit value that changes all at once (the pad word): the output only
// follows once two successive samples agree, so a word caught mid-change is
// never passed on.
module taiko_cdc_word #(
    parameter WIDTH = 16
) (
    input  wire             clk,
    input  wire [WIDTH-1:0] d,
    output reg  [WIDTH-1:0] q = 0
);
  reg [WIDTH-1:0] s0 = 0, s1 = 0, s2 = 0;
  always @(posedge clk) begin
    s0 <= d;
    s1 <= s0;
    s2 <= s1;
    if (s1 == s2) q <= s2;
  end
endmodule

// Snapshot of a wide bus from clk_a to clk_b with a request/acknowledge
// handshake: `data_a` is copied to a holding register that stays put while
// clk_b picks it up. `valid_b` pulses when `data_b` has a new value.
module taiko_cdc_snapshot #(
    parameter WIDTH = 32
) (
    input  wire             clk_a,
    input  wire [WIDTH-1:0] data_a,
    input  wire             clk_b,
    output reg  [WIDTH-1:0] data_b = 0,
    output reg              valid_b = 0
);
  reg [WIDTH-1:0] hold = 0;
  reg req = 0;
  reg [1:0] ack_sync = 0;
  reg [2:0] req_sync = 0;
  reg ack = 0;

  always @(posedge clk_a) begin
    ack_sync <= {ack_sync[0], ack};
    if (ack_sync[1] == req) begin  // the last snapshot was taken: send the next
      hold <= data_a;
      req  <= ~req;
    end
  end

  always @(posedge clk_b) begin
    req_sync <= {req_sync[1:0], req};
    valid_b  <= 0;
    if (req_sync[2] != req_sync[1]) begin
      data_b  <= hold;
      valid_b <= 1;
      ack     <= req_sync[1];
    end
  end
endmodule

// Small asynchronous FIFO (gray-coded pointers). `full` and `empty` are
// conservative, as usual for this structure.
module taiko_afifo #(
    parameter WIDTH = 37,
    parameter ADDR  = 6
) (
    input  wire             wr_clk,
    input  wire             wr_en,
    input  wire [WIDTH-1:0] wr_data,
    output wire             full,

    input  wire             rd_clk,
    input  wire             rd_en,
    output wire [WIDTH-1:0] rd_data,
    output wire             empty
);
  reg [WIDTH-1:0] mem[0:(1<<ADDR)-1];
  reg [ADDR:0] wr_bin = 0, wr_gray = 0, rd_bin = 0, rd_gray = 0;
  reg [ADDR:0] rd_gray_w0 = 0, rd_gray_w1 = 0, wr_gray_r0 = 0, wr_gray_r1 = 0;

  wire [ADDR:0] wr_bin_next = wr_bin + 1'b1;
  wire [ADDR:0] rd_bin_next = rd_bin + 1'b1;

  assign full  = wr_gray == {~rd_gray_w1[ADDR:ADDR-1], rd_gray_w1[ADDR-2:0]};
  assign empty = rd_gray == wr_gray_r1;
  assign rd_data = mem[rd_bin[ADDR-1:0]];

  always @(posedge wr_clk) begin
    rd_gray_w0 <= rd_gray;
    rd_gray_w1 <= rd_gray_w0;
    if (wr_en && !full) begin
      mem[wr_bin[ADDR-1:0]] <= wr_data;
      wr_bin  <= wr_bin_next;
      wr_gray <= wr_bin_next ^ (wr_bin_next >> 1);
    end
  end

  always @(posedge rd_clk) begin
    wr_gray_r0 <= wr_gray;
    wr_gray_r1 <= wr_gray_r0;
    if (rd_en && !empty) begin
      rd_bin  <= rd_bin_next;
      rd_gray <= rd_bin_next ^ (rd_bin_next >> 1);
    end
  end
endmodule

// Memory written in one clock domain and read in another (or the same). Infers block
// RAM. An M10K holds 10 kbit: 512 x 16 for the hit samples (32768 samples take 64 of
// the 308 blocks), 2048 x 5 for the sprite sheet.
module taiko_ram #(
    parameter ADDR = 15,
    parameter WIDTH = 16,
    parameter DEPTH = 1 << ADDR
) (
    input  wire             wr_clk,
    input  wire             wr_en,
    input  wire [ ADDR-1:0] wr_addr,
    input  wire [WIDTH-1:0] wr_data,

    input  wire             rd_clk,
    input  wire [ ADDR-1:0] rd_addr,
    output reg  [WIDTH-1:0] rd_data
);
  reg [WIDTH-1:0] mem[0:DEPTH-1];
  always @(posedge wr_clk) if (wr_en) mem[wr_addr] <= wr_data;
  always @(posedge rd_clk) rd_data <= mem[rd_addr];
endmodule
