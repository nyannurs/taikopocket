// Taiko Pocket sprite layer (GPL-3.0).
//
// Everything that moves in the lane (notes, bar lines, note names, the hit burst,
// the judgment text) is drawn here instead of by the CPU. The soft CPU needs about
// six cycles to copy a pixel into SDRAM and forty to blend one, which is most of a
// frame for a screen full of notes; this draws a pixel per cycle from block RAM and
// mixes it into the picture on its way to the screen.
//
// Memories, all written by the CPU through one address/data port:
//   sheet    73728 x 5 bit   pixels of all cells; 0 is transparent
//   palette  16 banks x 32   {alpha 0..16, colour RGB565 already multiplied by alpha}
//   cells    256             {double, clip, h[6:0], w[6:0], sheet address / 2 [15:0]}
//                            double: every pixel is shown 2 x 2 (soft, large pictures)
//   sprites  2 lists x 128   {bank[3:0], cell[7:0], y[9:0], x[9:0]}, x and y signed
//
// Sprites are drawn front to back: entry 0 is on top, and a pixel that has been drawn
// on a line is not drawn again. If a line runs out of time, the sprites at the back
// are the ones left out. A translucent pixel is mixed with the frame buffer under it,
// not with a sprite under it.
//
// Timing: one line of sprites is drawn into a line buffer two lines ahead of its
// display (two buffers, used alternately; a buffer is cleared before it is drawn into).
// A line is 2380 system clock cycles: 400 to clear, three per sprite in the list, one
// per pixel looked at. Two things keep the pixels looked at few when notes are piled
// up, which is when a line has many sprites (version 3):
//   - Sheet value 31 ends a sprite's row: nothing right of it is read. The pixels
//     after it in the row must be 0. A cell uses it after the last pixel of a row.
//   - `run` is a stretch of the line known to be drawn without a gap, the one the
//     last drawn pixels belong to. A sprite that reaches it jumps to its end in one
//     cycle (not a doubled one). A note behind another one then costs its visible
//     sliver, not its width.
//   - A row begins and ends where it can be seen: on the screen, and for a clipped
//     cell in the clip window.
// Without them a line had room for some 35 notes of 48 pixels.
// The CPU fills the list that is not in use and names it with `page`; the switch
// happens in the vertical blank.
module taiko_sprites #(
    parameter HRES = 400,
    parameter VRES = 360,
    parameter SHEET = 73728
) (
    input wire clk_sys,

    // CPU side
    input wire        mem_wr,
    input wire [ 1:0] mem_target,  // 0 sheet, 1 palette, 2 cells, 3 sprites
    input wire [16:0] mem_addr,
    input wire [31:0] mem_data,
    input wire        enable,
    input wire        page,
    input wire [ 7:0] count,       // sprites in list `page`
    output wire       page_shown,  // the list being drawn from: `page`, taken over at the start of a frame
    input wire [ 9:0] clip_x0,     // cells with the clip bit are only drawn in [x0, x1)
    input wire [ 9:0] clip_x1,

    // video side: the picture goes through here, two pixel clocks late
    input  wire        clk_vid,
    input  wire [23:0] rgb_in,
    input  wire        de_in,
    input  wire        hs_in,
    input  wire        vs_in,
    output reg  [23:0] rgb_out = 0,
    output reg         de_out = 0,
    output reg         hs_out = 0,
    output reg         vs_out = 0
);
  // ---- memories ----------------------------------------------------------------------------
  wire [16:0] sheet_addr;
  wire [ 4:0] sheet_q;
  taiko_ram #(.ADDR(17), .WIDTH(5), .DEPTH(SHEET)) sheet (
      .wr_clk(clk_sys), .wr_en(mem_wr && mem_target == 2'd0), .wr_addr(mem_addr), .wr_data(mem_data[4:0]),
      .rd_clk(clk_sys), .rd_addr(sheet_addr), .rd_data(sheet_q)
  );

  wire [ 8:0] pal_addr;
  wire [20:0] pal_q;  // {alpha[4:0], colour[15:0]}
  taiko_ram #(.ADDR(9), .WIDTH(21)) palette (
      .wr_clk(clk_sys), .wr_en(mem_wr && mem_target == 2'd1), .wr_addr(mem_addr[8:0]), .wr_data(mem_data[20:0]),
      .rd_clk(clk_sys), .rd_addr(pal_addr), .rd_data(pal_q)
  );

  wire [ 7:0] cell_addr;
  wire [31:0] cell_q;  // {double, clip, h, w, addr / 2}
  taiko_ram #(.ADDR(8), .WIDTH(32)) cells (
      .wr_clk(clk_sys), .wr_en(mem_wr && mem_target == 2'd2), .wr_addr(mem_addr[7:0]), .wr_data(mem_data),
      .rd_clk(clk_sys), .rd_addr(cell_addr), .rd_data(cell_q)
  );

  wire [ 7:0] spr_addr;
  wire [31:0] spr_q;  // {bank, cell, y, x}
  taiko_ram #(.ADDR(8), .WIDTH(32)) sprites (
      .wr_clk(clk_sys), .wr_en(mem_wr && mem_target == 2'd3), .wr_addr(mem_addr[7:0]), .wr_data(mem_data),
      .rd_clk(clk_sys), .rd_addr(spr_addr), .rd_data(spr_q)
  );

  // ---- video clock: which line to draw next -------------------------------------------------
  // A request goes out at the start of every horizontal sync: after active line k for
  // line k + 2, and for lines 0 and 1 on the first two lines after the vertical sync.
  reg hs_prev = 0, vs_prev = 0, line_active = 0;
  reg [8:0] line = 0;        // active line being displayed (or the next one)
  reg [1:0] first_lines = 0; // requests for lines 0 and 1 still to send
  reg       req_toggle = 0;
  reg [8:0] req_y = 0;
  reg       req_first = 0;
  wire [9:0] ahead = {1'b0, line} + 10'd2;

  always @(posedge clk_vid) begin
    hs_prev <= hs_in;
    vs_prev <= vs_in;
    if (de_in) line_active <= 1;
    if (vs_prev && !vs_in) begin
      line        <= 0;
      first_lines <= 2'd2;
    end
    if (hs_in && !hs_prev) begin
      line_active <= 0;
      if (line_active) begin
        line <= line + 9'd1;
        if (ahead < VRES) begin
          req_y      <= ahead[8:0];
          req_first  <= 0;
          req_toggle <= ~req_toggle;
        end
      end else if (first_lines != 0) begin
        req_y       <= (first_lines == 2'd2) ? 9'd0 : 9'd1;
        req_first   <= (first_lines == 2'd2);
        req_toggle  <= ~req_toggle;
        first_lines <= first_lines - 2'd1;
      end
    end
  end

  // ---- system clock: draw one line -----------------------------------------------------------
  reg [2:0] req_sync = 0;
  wire req = req_sync[2] ^ req_sync[1];
  always @(posedge clk_sys) req_sync <= {req_sync[1:0], req_toggle};

  localparam S_IDLE = 3'd0, S_ATTR = 3'd1, S_CELL = 3'd2, S_TEST = 3'd3, S_PIX = 3'd4, S_CLEAR = 3'd5;
  reg [8:0] clear_x = 0;
  reg       draw_after = 0;  // there are sprites to draw once the buffer is cleared
  reg [2:0] state = S_IDLE;
  reg [8:0] draw_y = 0;
  reg       page_l = 0;
  reg [7:0] count_l = 0;
  reg [7:0] index = 0;
  reg signed [10:0] spr_x = 0, spr_y = 0;
  reg [3:0] spr_bank = 0;
  reg [16:0] cur_addr = 0;
  reg signed [10:0] cur_x = 0;
  reg [7:0] left = 0;
  reg cur_clip = 0, cur_double = 0, second = 0;

  assign sheet_addr = cur_addr;
  assign page_shown = page_l;
  assign spr_addr  = {page_l, index[6:0]};
  assign cell_addr = spr_q[27:20];

  wire [6:0] cell_w = cell_q[22:16], cell_h = cell_q[29:23];
  wire cell_double = cell_q[31];
  wire signed [10:0] dy = $signed({2'b0, draw_y}) - spr_y;
  wire [7:0] shown_h = cell_double ? {cell_h, 1'b0} : {1'b0, cell_h};
  wire row_hit = !dy[10] && dy[9:0] < {2'd0, shown_h} && cell_w != 0;
  wire [6:0] sheet_row = cell_double ? dy[7:1] : dy[6:0];
  wire [13:0] row_offset = sheet_row * cell_w;

  // pixel pipeline: in S_PIX the sheet is read at cur_addr; p1 = its pixel is out and the
  // palette is being read; p2 = the colour is out and goes into the line buffer
  reg p1_valid = 0, p2_valid = 0, p2_seen = 0, p1_clip = 0, p2_clip = 0;
  reg signed [10:0] p1_x = 0, p2_x = 0;
  reg [3:0] p1_bank = 0;
  assign pal_addr = {p1_bank, sheet_q};

  reg [511:0] filled = 0;  // pixels of this line that are drawn
  wire [9:0] wx = p2_x[9:0];
  wire on_screen = !p2_x[10] && wx < HRES;
  wire in_clip = !p2_clip || (wx >= clip_x0 && wx < clip_x1);
  wire lb_we = p2_valid && on_screen && in_clip && !filled[wx[8:0]] && pal_q[20:16] != 0;

  // the end of a row: the pixel just read from the sheet is the marker
  localparam [4:0] ROW_END = 5'd31;
  wire row_end = p1_valid && sheet_q == ROW_END;

  // [run_lo, run_hi): pixels of this line that are all drawn. A pixel that is looked at
  // and is drawn (now or before) lengthens the run if it is next to its end, and starts
  // a new one if it is outside it.
  reg signed [10:0] run_lo = 0, run_hi = 0;
  wire drawn_here = p2_seen && on_screen && (filled[wx[8:0]] || lb_we);
  wire in_run = !cur_double && cur_x >= run_lo && cur_x < run_hi;
  // where a row can be seen: the screen, or the clip window for a cell that is clipped
  wire signed [10:0] left_edge = cur_clip ? $signed({1'b0, clip_x0}) : 11'sd0;
  wire before_left = !cur_double && cur_x < left_edge;
  wire past_right = !cur_x[10] && (cur_x[9:0] >= HRES || (cur_clip && cur_x[9:0] >= clip_x1));
  wire signed [10:0] jump_to = before_left ? left_edge : run_hi;
  wire [10:0] skip = jump_to - cur_x;  // 1 or more while before_left or in_run

  always @(posedge clk_sys) begin
    p1_valid <= 0;
    p2_valid <= p1_valid && sheet_q != 0 && sheet_q != ROW_END;
    p2_seen  <= p1_valid;
    p2_x     <= p1_x;
    p2_clip  <= p1_clip;
    if (lb_we) filled[wx[8:0]] <= 1;
    if (drawn_here) begin
      if (p2_x == run_hi) begin
        run_hi <= p2_x + 11'sd1;
      end else if (p2_x < run_lo || p2_x > run_hi) begin
        run_lo <= p2_x;
        run_hi <= p2_x + 11'sd1;
      end
    end

    case (state)
      S_ATTR: state <= S_CELL;  // the sprite entry is being read
      S_CELL: begin             // entry valid, the cell is being read
        spr_x    <= $signed({spr_q[9], spr_q[9:0]});
        spr_y    <= $signed({spr_q[19], spr_q[19:10]});
        spr_bank <= spr_q[31:28];
        state    <= S_TEST;
      end
      S_TEST: begin
        if (row_hit) begin
          cur_addr   <= {cell_q[15:0], 1'b0} + {3'd0, row_offset};
          cur_x      <= spr_x;
          left       <= cell_double ? {cell_w, 1'b0} : {1'b0, cell_w};
          cur_clip   <= cell_q[30];
          cur_double <= cell_double;
          second     <= 0;
          state    <= S_PIX;
        end else if (index + 8'd1 >= count_l) begin
          state <= S_IDLE;
        end else begin
          index <= index + 8'd1;
          state <= S_ATTR;
        end
      end
      S_PIX: begin
        if (in_run || before_left || past_right) begin  // nothing to draw here: over it in one step
          if (past_right || skip >= {3'd0, left}) begin
            if (index + 8'd1 >= count_l) begin
              state <= S_IDLE;
            end else begin
              index <= index + 8'd1;
              state <= S_ATTR;
            end
          end else begin
            cur_addr <= cur_addr + {6'd0, skip};
            cur_x    <= jump_to;
            left     <= left - skip[7:0];
          end
        end else begin
          p1_valid   <= 1;
          p1_x       <= cur_x;
          p1_bank    <= spr_bank;
          p1_clip    <= cur_clip;
          second     <= ~second;  // a doubled pixel is read twice
          if (!cur_double || second) cur_addr <= cur_addr + 17'd1;
          cur_x      <= cur_x + 11'sd1;
          left       <= left - 8'd1;
          if (left == 8'd1) begin
            if (index + 8'd1 >= count_l) begin
              state <= S_IDLE;
            end else begin
              index <= index + 8'd1;
              state <= S_ATTR;
            end
          end
        end
      end
      S_CLEAR: begin  // every pixel of the buffer to "nothing here"
        clear_x <= clear_x + 9'd1;
        if (clear_x == HRES - 1) state <= draw_after ? S_ATTR : S_IDLE;
      end
      default: ;
    endcase

    // the row ends here: the pixel being read now is dropped and the next sprite begins
    if (row_end && state == S_PIX) begin
      p1_valid <= 0;
      if (index + 8'd1 >= count_l) begin
        state <= S_IDLE;
      end else begin
        index <= index + 8'd1;
        state <= S_ATTR;
      end
    end

    if (req) begin  // a new line: whatever was left of the last one is dropped
      draw_y   <= req_y;
      index    <= 0;
      filled   <= 0;
      run_lo   <= 0;
      run_hi   <= 0;
      p2_seen  <= 0;
      p1_valid <= 0;
      p2_valid <= 0;
      if (req_first) begin
        page_l  <= page;
        count_l <= count;
      end
      // the list in use this frame is the one latched on line 0
      draw_after <= enable && (req_first ? count : count_l) != 0;
      clear_x    <= 0;
      state      <= S_CLEAR;
    end
  end

  // ---- line buffers ---------------------------------------------------------------------------
  // buffer n holds the lines with (y & 1) == n; written here, read by the video side
  reg [8:0] vid_x = 0;
  wire [20:0] lb_q[0:1];
  wire clearing = state == S_CLEAR;
  wire [8:0] lb_addr = clearing ? clear_x : wx[8:0];
  wire [20:0] lb_data = clearing ? 21'd0 : pal_q;
  genvar n;
  generate
    for (n = 0; n < 2; n = n + 1) begin : lb
      taiko_ram #(.ADDR(9), .WIDTH(21)) buffer (
          .wr_clk (clk_sys),
          .wr_en  ((clearing || lb_we) && draw_y[0] == n),
          .wr_addr(lb_addr),
          .wr_data(lb_data),
          .rd_clk (clk_vid),
          .rd_addr(vid_x),
          .rd_data(lb_q[n])
      );
    end
  endgenerate

  // ---- video clock: mix into the picture -------------------------------------------------------
  // out = sprite colour (already multiplied by its alpha) + picture * (16 - alpha) / 16
  reg [23:0] rgb_1 = 0;
  reg de_1 = 0, hs_1 = 0, vs_1 = 0, sel_1 = 0;
  wire [20:0] px = lb_q[sel_1];
  wire [4:0] alpha = de_1 ? px[20:16] : 5'd0;
  wire [4:0] keep = 5'd16 - (alpha > 5'd16 ? 5'd16 : alpha);
  // RGB565 to 8 bits per channel exactly as the picture is widened (rgb565_to_rgb888.sv)
  wire [13:0] sr14 = {2'b0, px[15:11]} * 10'd527 + 14'd23;
  wire [13:0] sg14 = {1'b0, px[10:5]} * 10'd259 + 14'd33;
  wire [13:0] sb14 = {2'b0, px[4:0]} * 10'd527 + 14'd23;
  wire [7:0] sr = sr14[13:6], sg = sg14[13:6], sb = sb14[13:6];

  function automatic [7:0] mix(input [7:0] sprite, input [7:0] picture, input [4:0] k);
    reg [12:0] scaled;
    reg [ 8:0] sum;
    begin
      scaled = picture * k;
      sum    = {1'b0, sprite} + {1'b0, scaled[11:4]};
      mix    = sum[8] ? 8'hFF : sum[7:0];
    end
  endfunction

  always @(posedge clk_vid) begin
    vid_x <= de_in ? vid_x + 9'd1 : 9'd0;
    rgb_1 <= rgb_in;
    de_1  <= de_in;
    hs_1  <= hs_in;
    vs_1  <= vs_in;
    sel_1 <= line[0];

    rgb_out <= (alpha == 0) ? rgb_1
               : {mix(sr, rgb_1[23:16], keep), mix(sg, rgb_1[15:8], keep), mix(sb, rgb_1[7:0], keep)};
    de_out  <= de_1;
    hs_out  <= hs_1;
    vs_out  <= vs_1;
  end
endmodule
