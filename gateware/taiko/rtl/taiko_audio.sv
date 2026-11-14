// Taiko Pocket audio core (GPL-3.0). Runs in the 12.288 MHz audio clock domain.
//
//  - music: pops one stereo sample per 48 kHz tick from the music FIFO and keeps
//    the exact song position (`pos`). It waits (position held) when the FIFO
//    runs dry mid-song, and keeps counting in silence once the song is finished.
//  - hit sounds: four voices playing don/ka samples from block RAM, started by
//    button press edges (`hit_auto`) or by the CPU, with no CPU in the path.
//  - mixer: music * gain * volume + hits * volume, saturated to 16 bits.
//  - input: every button edge is reported with the value of the free-running
//    sample counter, so the game can judge hits by when they happened.
//
// src/platform/common/softmix.c is the software model of this module; the test
// bench checks the two sample for sample.
//
// Latency: a trigger seen up to COMPUTE_AT cycles into a sample period is in
// the sample that is handed to the I2S serialiser at the next tick.
module taiko_audio #(
    parameter HIT_ADDR = 14,            // samples per hit sound = 2**HIT_ADDR
    parameter [7:0] COMPUTE_AT = 8'd200 // cycle of the 256-cycle period at which a sample is computed
) (
    input wire clk,
    input wire tick,  // one cycle per output sample

    // music FIFO, read side (first word fall-through is not assumed: q is valid the cycle after rdreq)
    input  wire [31:0] fifo_q,  // {left, right}
    input  wire        fifo_empty,
    output reg         fifo_rdreq = 0,

    // control, synchronous to clk
    input wire        run,
    input wire        finished,
    input wire        load,       // pulse: take start_pos and lead_in
    input wire [47:0] start_pos,  // signed song position of the next sample
    input wire [31:0] lead_in,    // samples of silence before the stream starts
    input wire [15:0] gain,       // song gain, 8.8
    input wire [15:0] vol_music,  // 8.8
    input wire [15:0] vol_hits,   // 8.8

    input wire        hit_auto,   // buttons trigger hit sounds
    input wire [15:0] don_mask,
    input wire [15:0] ka_mask,
    input wire        trig_don,   // CPU triggers (pulses)
    input wire        trig_ka,
    input wire [HIT_ADDR:0] hit_len0,   // samples in slot 0 (don) and 1 (ka)
    input wire [HIT_ADDR:0] hit_len1,
    input wire [31:0] hit_step0,  // 16.16 source samples per output sample
    input wire [31:0] hit_step1,

    input wire [15:0] keys,      // button word, synchronised
    input wire [11:0] debounce,  // samples a button must stay up before its release counts
    output reg [15:0] keys_db = 0,  // the buttons as the game sees them

    // hit sample RAM (rd_data one cycle after rd_addr)
    output reg  [HIT_ADDR:0] ram_addr = 0,
    input  wire [      15:0] ram_q,

    // to the I2S serialiser
    output reg signed [15:0] out_l = 0,
    output reg signed [15:0] out_r = 0,

    // status
    output reg [47:0] counter = 0,  // samples output since power-up
    output reg [47:0] pos = 0,
    output reg        starved = 0,

    // button edge events
    output reg        ev_valid = 0,
    output reg [ 3:0] ev_button = 0,
    output reg        ev_pressed = 0,
    output reg [31:0] ev_time = 0
);
  // ---- input edges -------------------------------------------------------------------------
  // Debounce. A press is taken the moment it is seen, so it is never late. A release
  // only counts once the button has stayed up for `debounce` samples: contact chatter
  // and short drop-outs while a button is held cannot turn one press into two.
  reg [11:0] up_left[0:15];
  integer b;
  initial for (b = 0; b < 16; b = b + 1) up_left[b] = 0;
  always @(posedge clk) begin
    for (b = 0; b < 16; b = b + 1) begin
      if (keys[b]) begin
        keys_db[b] <= 1;
        up_left[b] <= debounce;
      end else if (keys_db[b]) begin
        if (up_left[b] == 0) keys_db[b] <= 0;
        else if (tick) up_left[b] <= up_left[b] - 12'd1;
      end
    end
  end

  reg [15:0] keys_prev = 0, pend_press = 0, pend_release = 0;
  reg don_req = 0, ka_req = 0;
  wire [15:0] rising = keys_db & ~keys_prev;
  wire [15:0] falling = ~keys_db & keys_prev;
  wire [15:0] pend_any = pend_press | pend_release;

  function automatic [3:0] lowest(input [15:0] v);
    integer i;
    begin
      lowest = 0;
      for (i = 15; i >= 0; i = i - 1) if (v[i]) lowest = i[3:0];
    end
  endfunction
  wire [3:0] ev_index = lowest(pend_any);

  // ---- voices ------------------------------------------------------------------------------
  reg        v_active[0:3];
  reg        v_slot  [0:3];
  reg [31:0] v_phase [0:3];
  reg [ 1:0] next_voice = 0;
  integer k;
  initial for (k = 0; k < 4; k = k + 1) begin
    v_active[k] = 0;
    v_slot[k]   = 0;
    v_phase[k]  = 0;
  end

  // ---- per-sample computation ----------------------------------------------------------------
  reg [7:0] cycle = 0;  // cycles since the tick
  reg [3:0] step = 0;   // 0 idle, 1.. sequence
  reg [31:0] lead = 0;
  reg signed [15:0] mus_l = 0, mus_r = 0;
  reg popped = 0;
  reg signed [18:0] acc = 0;
  reg signed [15:0] next_l = 0, next_r = 0;
  reg [1:0] vi = 0;

  wire [HIT_ADDR:0] cur_len = v_slot[vi] ? hit_len1 : hit_len0;
  wire [31:0] cur_step = v_slot[vi] ? hit_step1 : hit_step0;
  wire [15:0] cur_idx = v_phase[vi][31:16];
  wire cur_plays = v_active[vi] && cur_idx < {{(15 - HIT_ADDR) {1'b0}}, cur_len};

  // music * gain >> 8, * volume >> 8; hits * volume >> 8 (arithmetic shifts, as in softmix.c)
  wire signed [33:0] gl = mus_l * $signed({1'b0, gain});
  wire signed [33:0] gr = mus_r * $signed({1'b0, gain});
  reg signed [25:0] gl_q = 0, gr_q = 0;
  wire signed [42:0] ml = gl_q * $signed({1'b0, vol_music});
  wire signed [42:0] mr = gr_q * $signed({1'b0, vol_music});
  wire signed [35:0] hv = acc * $signed({1'b0, vol_hits});
  reg signed [34:0] ml_q = 0, mr_q = 0;
  reg signed [27:0] hv_q = 0;
  wire signed [35:0] sum_l = {ml_q[34], ml_q} + {{8{hv_q[27]}}, hv_q};
  wire signed [35:0] sum_r = {mr_q[34], mr_q} + {{8{hv_q[27]}}, hv_q};

  function automatic signed [15:0] sat16(input signed [35:0] v);
    begin
      if (v > 36'sd32767) sat16 = 16'sd32767;
      else if (v < -36'sd32768) sat16 = 16'sh8000;
      else sat16 = v[15:0];
    end
  endfunction

  always @(posedge clk) begin
    fifo_rdreq <= 0;
    ev_valid   <= 0;

    // button edges: remember them, report one per cycle, lowest bit first
    keys_prev <= keys_db;
    pend_press <= pend_press | rising;
    pend_release <= pend_release | falling;
    if (hit_auto && |(rising & don_mask)) don_req <= 1;
    else if (hit_auto && |(rising & ka_mask)) ka_req <= 1;
    if (trig_don) don_req <= 1;
    if (trig_ka) ka_req <= 1;
    if (|pend_any) begin
      ev_valid   <= 1;
      ev_button  <= ev_index;
      ev_pressed <= pend_press[ev_index];
      ev_time    <= counter[31:0];
      // a press and a release of the same button waiting together go out press first
      if (pend_press[ev_index]) pend_press[ev_index] <= rising[ev_index];
      else pend_release[ev_index] <= falling[ev_index];
    end

    // start a voice (round robin: with all four busy the oldest restarts);
    // not while the voices are being read out for this sample
    if ((don_req || ka_req) && step == 4'd0) begin
      v_active[next_voice] <= 1;
      v_slot[next_voice]   <= don_req ? 1'b0 : 1'b1;
      v_phase[next_voice]  <= 0;
      next_voice           <= next_voice + 1'b1;
      if (don_req) don_req <= trig_don || (hit_auto && |(rising & don_mask));
      else ka_req <= trig_ka || (hit_auto && |(rising & ka_mask) && !(|(rising & don_mask)));
    end

    if (load) begin
      pos     <= start_pos;
      lead    <= lead_in;
      starved <= 0;
    end

    // one output sample per tick
    cycle <= cycle + 1'b1;
    if (tick) begin
      cycle   <= 0;
      counter <= counter + 1'b1;
      out_l   <= next_l;
      out_r   <= next_r;
    end

    case (step)
      4'd0:
      if (cycle == COMPUTE_AT && !tick) begin
        step   <= 4'd1;
        mus_l  <= 0;
        mus_r  <= 0;
        popped <= 0;
        acc    <= 0;
        vi     <= 0;
        if (run && !load) begin
          starved <= 0;
          if (lead != 0) begin
            lead <= lead - 1'b1;
            pos  <= pos + 1'b1;
          end else if (!fifo_empty) begin
            fifo_rdreq <= 1;
            popped     <= 1;
            pos        <= pos + 1'b1;
          end else if (finished) begin
            pos <= pos + 1'b1;
          end else begin
            starved <= 1;
          end
        end
      end
      4'd1: step <= 4'd2;  // FIFO read latency
      4'd2: begin
        if (popped) begin
          mus_l <= fifo_q[31:16];
          mus_r <= fifo_q[15:0];
        end
        step <= 4'd3;
      end
      // voices: address, wait, accumulate
      4'd3: begin
        ram_addr <= {v_slot[vi], cur_idx[HIT_ADDR-1:0]};
        step     <= 4'd4;
      end
      4'd4: step <= 4'd5;
      4'd5: begin
        if (cur_plays) begin
          acc         <= acc + {{3{ram_q[15]}}, ram_q};
          v_phase[vi] <= v_phase[vi] + cur_step;
        end else begin
          v_active[vi] <= 0;
        end
        vi   <= vi + 1'b1;
        step <= (vi == 2'd3) ? 4'd6 : 4'd3;
      end
      4'd6: begin
        gl_q <= gl[33:8];
        gr_q <= gr[33:8];
        step <= 4'd7;
      end
      4'd7: begin
        ml_q <= ml[42:8];
        mr_q <= mr[42:8];
        hv_q <= hv[35:8];
        step <= 4'd8;
      end
      4'd8: begin
        next_l <= sat16(sum_l);
        next_r <= sat16(sum_r);
        step   <= 4'd0;
      end
      default: step <= 4'd0;
    endcase
  end
endmodule
