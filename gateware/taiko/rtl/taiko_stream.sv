// Taiko Pocket music streamer (GPL-3.0). System clock domain.
//
// Reads the song from a ring buffer in SDRAM over Wishbone, decodes it and
// feeds the music FIFO, so the CPU never touches a sample. The game fills the
// ring through the APF bridge (the bridge writes the file data straight into
// SDRAM) and tells this module how many bytes are there (`fed`).
//
// Formats: 16-bit PCM (mono or stereo) and IMA ADPCM in WAV block layout:
// per channel a header word {8'h0, step_index, predictor}, then 32-bit words
// of 8 nibbles (low nibble first), alternating channels. The predictor is the
// block's first sample.
module taiko_stream (
    input wire clk,
    input wire rst,

    // configuration (stable while `enable` is high)
    input wire        enable,
    input wire        start,        // pulse: restart from the beginning of the ring
    input wire [31:0] ring_base,    // byte address of the ring in memory
    input wire [31:0] ring_bytes,   // its size, a multiple of the block size
    input wire [31:0] fed,          // bytes written to the ring since `start`
    input wire        adpcm,        // 0: PCM, 1: IMA ADPCM
    input wire        stereo,
    input wire [15:0] block_words,  // ADPCM block size in 32-bit words
    input wire [31:0] skip,         // decoded samples to drop before the first one is output

    output reg [31:0] consumed = 0, // bytes read from the ring
    output wire       drained,      // everything fed so far has gone into the FIFO

    // Wishbone master (read only, classic cycles)
    output reg  [29:0] wb_adr = 0,
    output reg         wb_cyc = 0,
    output reg         wb_stb = 0,
    input  wire        wb_ack,
    input  wire [31:0] wb_dat_r,

    // music FIFO, write side
    output reg  [31:0] fifo_data = 0,  // {left, right}
    output reg         fifo_wrreq = 0,
    input  wire        fifo_full
);
  // ---- IMA ADPCM step -------------------------------------------------------------------
  function automatic [14:0] step_size(input [6:0] index);
    case (index)
      7'd0: step_size = 15'd7;       7'd1: step_size = 15'd8;       7'd2: step_size = 15'd9;
      7'd3: step_size = 15'd10;      7'd4: step_size = 15'd11;      7'd5: step_size = 15'd12;
      7'd6: step_size = 15'd13;      7'd7: step_size = 15'd14;      7'd8: step_size = 15'd16;
      7'd9: step_size = 15'd17;      7'd10: step_size = 15'd19;     7'd11: step_size = 15'd21;
      7'd12: step_size = 15'd23;     7'd13: step_size = 15'd25;     7'd14: step_size = 15'd28;
      7'd15: step_size = 15'd31;     7'd16: step_size = 15'd34;     7'd17: step_size = 15'd37;
      7'd18: step_size = 15'd41;     7'd19: step_size = 15'd45;     7'd20: step_size = 15'd50;
      7'd21: step_size = 15'd55;     7'd22: step_size = 15'd60;     7'd23: step_size = 15'd66;
      7'd24: step_size = 15'd73;     7'd25: step_size = 15'd80;     7'd26: step_size = 15'd88;
      7'd27: step_size = 15'd97;     7'd28: step_size = 15'd107;    7'd29: step_size = 15'd118;
      7'd30: step_size = 15'd130;    7'd31: step_size = 15'd143;    7'd32: step_size = 15'd157;
      7'd33: step_size = 15'd173;    7'd34: step_size = 15'd190;    7'd35: step_size = 15'd209;
      7'd36: step_size = 15'd230;    7'd37: step_size = 15'd253;    7'd38: step_size = 15'd279;
      7'd39: step_size = 15'd307;    7'd40: step_size = 15'd337;    7'd41: step_size = 15'd371;
      7'd42: step_size = 15'd408;    7'd43: step_size = 15'd449;    7'd44: step_size = 15'd494;
      7'd45: step_size = 15'd544;    7'd46: step_size = 15'd598;    7'd47: step_size = 15'd658;
      7'd48: step_size = 15'd724;    7'd49: step_size = 15'd796;    7'd50: step_size = 15'd876;
      7'd51: step_size = 15'd963;    7'd52: step_size = 15'd1060;   7'd53: step_size = 15'd1166;
      7'd54: step_size = 15'd1282;   7'd55: step_size = 15'd1411;   7'd56: step_size = 15'd1552;
      7'd57: step_size = 15'd1707;   7'd58: step_size = 15'd1878;   7'd59: step_size = 15'd2066;
      7'd60: step_size = 15'd2272;   7'd61: step_size = 15'd2499;   7'd62: step_size = 15'd2749;
      7'd63: step_size = 15'd3024;   7'd64: step_size = 15'd3327;   7'd65: step_size = 15'd3660;
      7'd66: step_size = 15'd4026;   7'd67: step_size = 15'd4428;   7'd68: step_size = 15'd4871;
      7'd69: step_size = 15'd5358;   7'd70: step_size = 15'd5894;   7'd71: step_size = 15'd6484;
      7'd72: step_size = 15'd7132;   7'd73: step_size = 15'd7845;   7'd74: step_size = 15'd8630;
      7'd75: step_size = 15'd9493;   7'd76: step_size = 15'd10442;  7'd77: step_size = 15'd11487;
      7'd78: step_size = 15'd12635;  7'd79: step_size = 15'd13899;  7'd80: step_size = 15'd15289;
      7'd81: step_size = 15'd16818;  7'd82: step_size = 15'd18500;  7'd83: step_size = 15'd20350;
      7'd84: step_size = 15'd22385;  7'd85: step_size = 15'd24623;  7'd86: step_size = 15'd27086;
      7'd87: step_size = 15'd29794;
      default: step_size = 15'd32767;
    endcase
  endfunction

  // one nibble: {new index, new predictor}
  function automatic [22:0] ima(input signed [15:0] pred, input [6:0] index, input [3:0] nib);
    reg [14:0] step;
    reg [16:0] diff;
    reg signed [18:0] next;
    reg signed [8:0] idx;
    begin
      step = step_size(index);
      diff = {5'd0, step[14:3]};
      if (nib[2]) diff = diff + {2'd0, step};
      if (nib[1]) diff = diff + {3'd0, step[14:1]};
      if (nib[0]) diff = diff + {4'd0, step[14:2]};
      next = nib[3] ? $signed({{3{pred[15]}}, pred}) - $signed({2'b0, diff})
                    : $signed({{3{pred[15]}}, pred}) + $signed({2'b0, diff});
      if (next > 19'sd32767) next = 19'sd32767;
      else if (next < -19'sd32768) next = -19'sd32768;
      case (nib[2:0])
        3'd4: idx = $signed({2'b0, index}) + 9'sd2;
        3'd5: idx = $signed({2'b0, index}) + 9'sd4;
        3'd6: idx = $signed({2'b0, index}) + 9'sd6;
        3'd7: idx = $signed({2'b0, index}) + 9'sd8;
        default: idx = $signed({2'b0, index}) - 9'sd1;
      endcase
      if (idx < 0) idx = 0;
      else if (idx > 9'sd88) idx = 9'sd88;
      ima = {idx[6:0], next[15:0]};
    end
  endfunction

  // ---- state ------------------------------------------------------------------------------
  localparam S_IDLE = 3'd0, S_READ = 3'd1, S_WORD = 3'd2, S_NIBBLE = 3'd3, S_PUSH = 3'd4;
  reg [2:0] state = S_IDLE;
  reg [31:0] offset = 0;       // read offset in the ring
  reg [31:0] skip_left = 0;
  reg [15:0] word_index = 0;   // word number in the ADPCM block
  reg [31:0] word = 0, word_l = 0;
  reg signed [15:0] pred_l = 0, pred_r = 0;
  reg [6:0] idx_l = 0, idx_r = 0;
  reg [3:0] nibble = 0;        // next nibble of the word(s), 8 = done
  reg second = 0;              // PCM mono: second sample of the word
  reg [2:0] after_push = S_IDLE;

  wire [31:0] available = fed - consumed;
  assign drained = state == S_IDLE && available < 32'd4 && !fifo_wrreq;
  wire [31:0] ring_addr = ring_base + offset;
  wire [22:0] dec_l = ima(pred_l, idx_l, stereo ? word_l[nibble[2:0]*4+:4] : word[nibble[2:0]*4+:4]);
  wire [22:0] dec_r = ima(pred_r, idx_r, word[nibble[2:0]*4+:4]);

  always @(posedge clk) begin
    fifo_wrreq <= 0;
    case (state)
      S_IDLE:
      if (enable && available >= 32'd4) begin
        wb_adr <= ring_addr[31:2];
        wb_cyc <= 1;
        wb_stb <= 1;
        state  <= S_READ;
      end
      S_READ:
      if (wb_ack) begin
        wb_cyc   <= 0;
        wb_stb   <= 0;
        word     <= wb_dat_r;
        consumed <= consumed + 32'd4;
        offset   <= (offset + 32'd4 >= ring_bytes) ? 32'd0 : offset + 32'd4;
        state    <= S_WORD;
      end
      S_WORD: begin  // what does this word mean?
        second <= 0;
        nibble <= 0;
        if (!adpcm) begin
          // PCM: stereo {right, left} or two mono samples, little endian
          fifo_data  <= stereo ? {word[15:0], word[31:16]} : {word[15:0], word[15:0]};
          after_push <= stereo ? S_IDLE : S_WORD;
          if (!stereo && second) begin
            fifo_data  <= {word[31:16], word[31:16]};
            after_push <= S_IDLE;
          end
          second <= !stereo && !second;
          state  <= S_PUSH;
        end else begin
          word_index <= (word_index + 16'd1 == block_words) ? 16'd0 : word_index + 16'd1;
          if (word_index == 16'd0) begin  // header of the left (or only) channel
            pred_l <= word[15:0];
            idx_l  <= (word[23:16] > 8'd88) ? 7'd88 : word[22:16];
            if (stereo) begin
              state <= S_IDLE;
            end else begin
              fifo_data  <= {word[15:0], word[15:0]};
              after_push <= S_IDLE;
              state      <= S_PUSH;
            end
          end else if (stereo && word_index == 16'd1) begin  // header of the right channel
            pred_r     <= word[15:0];
            idx_r      <= (word[23:16] > 8'd88) ? 7'd88 : word[22:16];
            fifo_data  <= {pred_l, word[15:0]};
            after_push <= S_IDLE;
            state      <= S_PUSH;
          end else if (stereo && !word_index[0]) begin  // left data: keep it for the right word
            word_l <= word;
            state  <= S_IDLE;
          end else begin
            state <= S_NIBBLE;
          end
        end
      end
      S_NIBBLE:
      if (nibble[3]) begin
        state <= S_IDLE;
      end else begin
        pred_l <= dec_l[15:0];
        idx_l  <= dec_l[22:16];
        if (stereo) begin
          pred_r    <= dec_r[15:0];
          idx_r     <= dec_r[22:16];
          fifo_data <= {dec_l[15:0], dec_r[15:0]};
        end else begin
          fifo_data <= {dec_l[15:0], dec_l[15:0]};
        end
        nibble     <= nibble + 4'd1;
        after_push <= S_NIBBLE;
        state      <= S_PUSH;
      end
      S_PUSH:
      if (skip_left != 0) begin
        skip_left <= skip_left - 32'd1;
        state     <= after_push;
      end else if (!fifo_full && !fifo_wrreq) begin
        fifo_wrreq <= 1;
        state      <= after_push;
      end
      default: state <= S_IDLE;
    endcase

    if (rst || start) begin
      state      <= S_IDLE;
      wb_cyc     <= 0;
      wb_stb     <= 0;
      consumed   <= 0;
      offset     <= 0;
      word_index <= 0;
      skip_left  <= skip;
      second     <= 0;
      fifo_wrreq <= 0;
    end
  end
endmodule
