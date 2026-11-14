// Taiko Pocket peripheral (GPL-3.0): register block on the SoC's external
// Wishbone port plus the audio, input and streaming logic behind it.
// Register map: gateware/taiko/README.md and src/platform/pocket/taiko_regs.h.
module taiko_top #(
    parameter HIT_ADDR = 14
) (
    input wire clk_sys,
    input wire rst,

    // Wishbone slave: the SoC's external region at 0x8000_0000 (word addresses)
    input  wire [29:0] wbs_adr,
    input  wire [31:0] wbs_dat_w,
    output reg  [31:0] wbs_dat_r,
    input  wire        wbs_cyc,
    input  wire        wbs_stb,
    input  wire        wbs_we,
    output reg         wbs_ack,

    // Wishbone master: the streamer reads the music ring from SDRAM
    output wire [29:0] wbm_adr,
    output wire        wbm_cyc,
    output wire        wbm_stb,
    input  wire        wbm_ack,
    input  wire [31:0] wbm_dat_r,

    // the SoC's own audio registers (CPU-fed music, used until the streamer is enabled)
    input wire [31:0] cpu_audio_data,
    input wire        cpu_audio_wr,
    input wire        cpu_playback_en,

    // music FIFO, write side (clk_sys)
    output wire [31:0] fifo_wr_data,
    output wire        fifo_wrreq,
    input  wire        fifo_wrfull,

    // audio clock domain
    input  wire        clk_audio,
    input  wire        tick,
    input  wire [31:0] fifo_q,
    input  wire        fifo_rdempty,
    output wire        fifo_rdreq,
    output wire [15:0] out_l,
    output wire [15:0] out_r,

    input wire [15:0] keys_async,    // button word of the pad controller
    input wire        vblank_async,  // high outside the active lines

    output reg [15:0] bridge_offset_hi,  // bits 47:32 of data slot read offsets
    output reg        video_on = 0,      // 0: the core shows black (until the game has set up the picture)

    // the picture on its way out, for the sprite layer (video clock)
    input  wire        clk_vid,
    input  wire [23:0] vid_rgb_in,
    input  wire        vid_de_in,
    input  wire        vid_hs_in,
    input  wire        vid_vs_in,
    output wire [23:0] vid_rgb_out,
    output wire        vid_de_out,
    output wire        vid_hs_out,
    output wire        vid_vs_out
);
  localparam [31:0] ID = 32'h54504B31;  // "TPK1"
  initial begin
    wbs_dat_r = 0;
    wbs_ack = 0;
    bridge_offset_hi = 0;
  end

  // ---- registers ---------------------------------------------------------------------------
  reg stream_enable = 0, run = 0, finished = 0, hit_auto = 0;
  reg [11:0] debounce = 0;
  // sprite layer
  reg        ovl_enable = 0, ovl_page = 0, ovl_wr = 0;
  reg [ 7:0] ovl_count = 0;
  reg [ 1:0] ovl_target = 0;
  reg [16:0] ovl_addr = 0;
  reg [31:0] ovl_data = 0;
  reg [ 9:0] ovl_clip_x0 = 0, ovl_clip_x1 = 10'd400;
  reg [15:0] gain = 16'd256, vol_music = 16'd256, vol_hits = 16'd256;
  reg [47:0] start_pos = 0;
  reg [31:0] lead_in = 0, skip = 0, ring_base = 0, ring_bytes = 0, fed = 0;
  reg adpcm = 0, stereo = 0;
  reg [15:0] block_words = 0;
  reg [HIT_ADDR:0] hit_wr_addr = 0, hit_len0 = 0, hit_len1 = 0;
  reg [31:0] hit_step0 = 32'h10000, hit_step1 = 32'h10000;
  reg [15:0] don_mask = 0, ka_mask = 0;
  reg cmd_begin = 0, cmd_don = 0, cmd_ka = 0;
  reg hit_wr = 0;
  reg [15:0] hit_wr_data = 0;
  reg [47:0] latched_counter = 0, latched_pos = 0;
  reg ev_overflow = 0;
  reg ev_pop = 0;

  // ---- status from the audio domain --------------------------------------------------------
  wire [47:0] a_counter, a_pos;
  wire a_starved;
  wire [96:0] snap;
  taiko_cdc_snapshot #(
      .WIDTH(97)
  ) status_snapshot (
      .clk_a  (clk_audio),
      .data_a ({a_starved, a_pos, a_counter}),
      .clk_b  (clk_sys),
      .data_b (snap),
      .valid_b()
  );

  wire [31:0] consumed;
  wire ev_empty;
  wire [36:0] ev_data;  // {pressed, button, time}
  wire [15:0] keys_sys;
  wire [15:0] a_keys_db_w;  // debounced in the audio domain, see taiko_audio
  taiko_cdc_word #(.WIDTH(16)) keys_to_sys (.clk(clk_sys), .d(a_keys_db_w), .q(keys_sys));

  wire vblank;
  reg vblank_prev = 0;
  reg [30:0] frame_count = 0;
  taiko_cdc_bit vblank_to_sys (.clk(clk_sys), .d(vblank_async), .q(vblank));

  wire sel = wbs_cyc && wbs_stb && !wbs_ack;
  wire [5:0] reg_index = wbs_adr[5:0];

  always @(posedge clk_sys) begin
    wbs_ack   <= 0;
    cmd_begin <= 0;
    cmd_don   <= 0;
    cmd_ka    <= 0;
    hit_wr    <= 0;
    ev_pop    <= 0;
    if (hit_wr) hit_wr_addr <= hit_wr_addr + 1'b1;
    ovl_wr <= 0;
    if (ovl_wr) ovl_addr <= ovl_addr + 17'd1;

    vblank_prev <= vblank;
    if (vblank && !vblank_prev) frame_count <= frame_count + 1'b1;

    if (sel) begin
      wbs_ack <= 1;
      if (wbs_we) begin
        case (reg_index)
          6'h01: {video_on, hit_auto, finished, run, stream_enable} <= wbs_dat_w[4:0];
          6'h02: begin
            cmd_begin <= wbs_dat_w[0];
            cmd_don   <= wbs_dat_w[1];
            cmd_ka    <= wbs_dat_w[2];
            if (wbs_dat_w[3]) begin
              latched_counter <= snap[47:0];
              latched_pos     <= snap[95:48];
            end
            if (wbs_dat_w[4]) ev_overflow <= 0;
          end
          6'h03: gain <= wbs_dat_w[15:0];
          6'h04: vol_music <= wbs_dat_w[15:0];
          6'h05: vol_hits <= wbs_dat_w[15:0];
          6'h06: start_pos[31:0] <= wbs_dat_w;
          6'h07: start_pos[47:32] <= wbs_dat_w[15:0];
          6'h08: lead_in <= wbs_dat_w;
          6'h09: skip <= wbs_dat_w;
          6'h0A: ring_base <= wbs_dat_w;
          6'h0B: ring_bytes <= wbs_dat_w;
          6'h0C: fed <= wbs_dat_w;
          6'h0E: begin
            adpcm       <= wbs_dat_w[0];
            stereo      <= wbs_dat_w[1];
            block_words <= wbs_dat_w[31:16];
          end
          6'h0F: hit_wr_addr <= wbs_dat_w[HIT_ADDR:0];
          6'h10: begin
            hit_wr      <= 1;
            hit_wr_data <= wbs_dat_w[15:0];
          end
          6'h11: hit_len0 <= wbs_dat_w[HIT_ADDR:0];
          6'h12: hit_len1 <= wbs_dat_w[HIT_ADDR:0];
          6'h13: hit_step0 <= wbs_dat_w;
          6'h14: hit_step1 <= wbs_dat_w;
          6'h21: debounce <= wbs_dat_w[11:0];
          6'h22: {ovl_count, ovl_page, ovl_enable} <= {wbs_dat_w[15:8], wbs_dat_w[1:0]};
          6'h23: {ovl_target, ovl_addr} <= {wbs_dat_w[17:16], 1'b0, wbs_dat_w[15:0]};
          6'h24: begin
            ovl_wr   <= 1;
            ovl_data <= wbs_dat_w;
          end
          6'h25: {ovl_clip_x1, ovl_clip_x0} <= {wbs_dat_w[25:16], wbs_dat_w[9:0]};
          6'h15: don_mask <= wbs_dat_w[15:0];
          6'h16: ka_mask <= wbs_dat_w[15:0];
          6'h20: bridge_offset_hi <= wbs_dat_w[15:0];
          default: ;
        endcase
      end else begin
        case (reg_index)
          6'h00: wbs_dat_r <= ID;
          6'h01: wbs_dat_r <= {27'd0, video_on, hit_auto, finished, run, stream_enable};
          6'h21: wbs_dat_r <= {20'd0, debounce};
          // 31:24: version of the sprite layer (2: pixel doubling, cell addresses in 2 px, 73728 px sheet;
          //        3: sheet value 31 ends a row, pixels that are drawn already are skipped)
          6'h22: wbs_dat_r <= {8'd3, 8'd0, ovl_count, 6'd0, ovl_page, ovl_enable};
          6'h03: wbs_dat_r <= {16'd0, gain};
          6'h04: wbs_dat_r <= {16'd0, vol_music};
          6'h05: wbs_dat_r <= {16'd0, vol_hits};
          6'h0C: wbs_dat_r <= fed;
          6'h0D: wbs_dat_r <= consumed;
          6'h17: wbs_dat_r <= latched_counter[31:0];
          6'h18: wbs_dat_r <= {16'd0, latched_counter[47:32]};
          6'h19: wbs_dat_r <= latched_pos[31:0];
          6'h1A: wbs_dat_r <= {{16{latched_pos[47]}}, latched_pos[47:32]};
          6'h1B: wbs_dat_r <= {29'd0, ev_overflow, !ev_empty, snap[96]};
          6'h1C: wbs_dat_r <= {!ev_empty, 26'd0, ev_data[36:32]};
          6'h1D: begin
            wbs_dat_r <= ev_data[31:0];
            ev_pop    <= !ev_empty;
          end
          6'h1E: wbs_dat_r <= {16'd0, keys_sys};
          6'h1F: wbs_dat_r <= {frame_count, vblank};
          6'h20: wbs_dat_r <= {16'd0, bridge_offset_hi};
          default: wbs_dat_r <= 32'd0;
        endcase
      end
    end
    if (ev_full_sys) ev_overflow <= 1;
    if (rst) begin
      {video_on, hit_auto, finished, run, stream_enable} <= 5'd0;
      ovl_enable <= 0;
      bridge_offset_hi <= 0;
    end
  end

  // ---- sprite layer ------------------------------------------------------------------------
  taiko_sprites sprite_layer (
      .clk_sys   (clk_sys),
      .mem_wr    (ovl_wr),
      .mem_target(ovl_target),
      .mem_addr  (ovl_addr),
      .mem_data  (ovl_data),
      .enable    (ovl_enable),
      .page      (ovl_page),
      .count     (ovl_count),
      .clip_x0   (ovl_clip_x0),
      .clip_x1   (ovl_clip_x1),
      .clk_vid   (clk_vid),
      .rgb_in    (vid_rgb_in),
      .de_in     (vid_de_in),
      .hs_in     (vid_hs_in),
      .vs_in     (vid_vs_in),
      .rgb_out   (vid_rgb_out),
      .de_out    (vid_de_out),
      .hs_out    (vid_hs_out),
      .vs_out    (vid_vs_out)
  );

  // ---- streamer ----------------------------------------------------------------------------
  wire [31:0] stream_data;
  wire stream_wrreq;
  taiko_stream stream (
      .clk        (clk_sys),
      .rst        (rst),
      .enable     (stream_enable),
      .start      (cmd_begin),
      .ring_base  (ring_base),
      .ring_bytes (ring_bytes),
      .fed        (fed),
      .adpcm      (adpcm),
      .stereo     (stereo),
      .block_words(block_words),
      .skip       (skip),
      .consumed   (consumed),
      .wb_adr     (wbm_adr),
      .wb_cyc     (wbm_cyc),
      .wb_stb     (wbm_stb),
      .wb_ack     (wbm_ack),
      .wb_dat_r   (wbm_dat_r),
      .fifo_data  (stream_data),
      .fifo_wrreq (stream_wrreq),
      .fifo_full  (fifo_wrfull)
  );
  assign fifo_wr_data = stream_enable ? stream_data : cpu_audio_data;
  assign fifo_wrreq   = stream_enable ? stream_wrreq : cpu_audio_wr;

  // ---- into the audio domain ---------------------------------------------------------------
  wire a_run, a_finished, a_hit_auto, a_load, a_trig_don, a_trig_ka;
  // without the streamer the SoC's playback_en starts and stops the music, and an
  // empty FIFO is just silence (the stock behaviour)
  taiko_cdc_bit #(.WIDTH(3)) ctrl_to_audio (
      .clk(clk_audio),
      .d  ({hit_auto, stream_enable ? finished : 1'b1, stream_enable ? run : cpu_playback_en}),
      .q  ({a_hit_auto, a_finished, a_run})
  );
  taiko_cdc_pulse begin_to_audio (.clk_a(clk_sys), .pulse_a(cmd_begin), .clk_b(clk_audio), .pulse_b(a_load));
  taiko_cdc_pulse don_to_audio (.clk_a(clk_sys), .pulse_a(cmd_don), .clk_b(clk_audio), .pulse_b(a_trig_don));
  taiko_cdc_pulse ka_to_audio (.clk_a(clk_sys), .pulse_a(cmd_ka), .clk_b(clk_audio), .pulse_b(a_trig_ka));

  wire [15:0] a_keys, a_keys_db;
  assign a_keys_db_w = a_keys_db;
  taiko_cdc_word #(.WIDTH(16)) keys_to_audio (.clk(clk_audio), .d(keys_async), .q(a_keys));

  wire [HIT_ADDR:0] ram_addr;
  wire [15:0] ram_q;
  taiko_ram #(
      .ADDR(HIT_ADDR + 1)
  ) hit_ram (
      .wr_clk (clk_sys),
      .wr_en  (hit_wr),
      .wr_addr(hit_wr_addr),
      .wr_data(hit_wr_data),
      .rd_clk (clk_audio),
      .rd_addr(ram_addr),
      .rd_data(ram_q)
  );

  wire a_ev_valid, a_ev_pressed;
  wire [3:0] a_ev_button;
  wire [31:0] a_ev_time;
  wire a_ev_full;
  wire ev_full_sys;
  taiko_cdc_bit ev_full_to_sys (.clk(clk_sys), .d(a_ev_full && a_ev_valid), .q(ev_full_sys));

  // The configuration words below cross as they are: they only change while the
  // CPU is setting something up, and `load` / the triggers arrive after them.
  taiko_audio #(
      .HIT_ADDR(HIT_ADDR)
  ) audio (
      .clk       (clk_audio),
      .tick      (tick),
      .fifo_q    (fifo_q),
      .fifo_empty(fifo_rdempty),
      .fifo_rdreq(fifo_rdreq),
      .run       (a_run),
      .finished  (a_finished),
      .load      (a_load),
      .start_pos (start_pos),
      .lead_in   (lead_in),
      .gain      (gain),
      .vol_music (vol_music),
      .vol_hits  (vol_hits),
      .hit_auto  (a_hit_auto),
      .don_mask  (don_mask),
      .ka_mask   (ka_mask),
      .trig_don  (a_trig_don),
      .trig_ka   (a_trig_ka),
      .hit_len0  (hit_len0),
      .hit_len1  (hit_len1),
      .hit_step0 (hit_step0),
      .hit_step1 (hit_step1),
      .keys      (a_keys),
      .debounce  (debounce),
      .keys_db   (a_keys_db),
      .ram_addr  (ram_addr),
      .ram_q     (ram_q),
      .out_l     (out_l),
      .out_r     (out_r),
      .counter   (a_counter),
      .pos       (a_pos),
      .starved   (a_starved),
      .ev_valid  (a_ev_valid),
      .ev_button (a_ev_button),
      .ev_pressed(a_ev_pressed),
      .ev_time   (a_ev_time)
  );

  taiko_afifo #(
      .WIDTH(37),
      .ADDR (6)
  ) events (
      .wr_clk (clk_audio),
      .wr_en  (a_ev_valid),
      .wr_data({a_ev_pressed, a_ev_button, a_ev_time}),
      .full   (a_ev_full),
      .rd_clk (clk_sys),
      .rd_en  (ev_pop),
      .rd_data(ev_data),
      .empty  (ev_empty)
  );
endmodule
