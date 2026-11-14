// Audio output for Taiko Pocket: the music FIFO, the 48 kHz tick and the I2S
// serialiser. What is played is decided by taiko_top (gateware/taiko), which
// reads the FIFO and hands back the mixed sample. Based on the openfpga-litex
// audio module (MIT, Adam Gastineau).
module audio (
    input wire clk_74b,
    input wire clk_sys,

    input wire reset,

    // music FIFO, write side (clk_sys)
    input  wire [31:0] fifo_wr_data,
    input  wire        fifo_wrreq,
    output wire        fifo_wrfull,
    input  wire        audio_flush,
    output wire [11:0] audio_buffer_fill,

    // audio clock domain (audio_mclk)
    output wire [31:0] fifo_q,
    output wire        fifo_rdempty,
    input  wire        fifo_rdreq,
    output reg         tick = 0,      // one cycle per 48 kHz sample
    input  wire [15:0] sample_l,
    input  wire [15:0] sample_r,

    output wire audio_mclk,  //! Serial Master Clock
    output wire audio_lrck,  //! Left/Right clock
    output wire audio_dac    //! Serialized data
);

  ////////////////////////////////////////////////////////////////////////////////////////
  // Audio PLL

  wire audio_sclk;

  mf_audio_pll audio_pll (
      .refclk  (clk_74b),
      .rst     (0),
      .outclk_0(audio_mclk),
      .outclk_1(audio_sclk)
  );

  ////////////////////////////////////////////////////////////////////////////////////////
  // FIFO

  dcfifo dcfifo_component (
      .wrclk(clk_sys),
      .rdclk(audio_mclk),

      .data (fifo_wr_data),
      .wrreq(fifo_wrreq),

      .q(fifo_q),
      .rdreq(fifo_rdreq),

      .rdempty(fifo_rdempty),
      .wrfull(fifo_wrfull),
      .wrusedw(audio_buffer_fill),

      .aclr(reset || audio_flush)
  );
  defparam dcfifo_component.intended_device_family = "Cyclone V",
      dcfifo_component.lpm_numwords = 4096, dcfifo_component.lpm_showahead = "OFF",
      dcfifo_component.lpm_type = "dcfifo", dcfifo_component.lpm_width = 32,
      dcfifo_component.lpm_widthu = 12, dcfifo_component.overflow_checking = "ON",
      dcfifo_component.rdsync_delaypipe = 5, dcfifo_component.underflow_checking = "ON",
      dcfifo_component.use_eab = "ON", dcfifo_component.wrsync_delaypipe = 5;

  reg [7:0] mclk_div = 8'hFF;

  always @(posedge audio_mclk) begin
    // MClk is 12.288 MHz, we want 48kHz
    tick <= 0;

    if (mclk_div > 0) begin
      mclk_div <= mclk_div - 8'h1;
    end else begin
      mclk_div <= 8'hFF;

      tick <= 1;
    end
  end

  ////////////////////////////////////////////////////////////////////////////////////////
  // i2s Generation

  sound_i2s #(
      .SIGNED_INPUT(1)
  ) sound_i2s (
      .audio_sclk(audio_sclk),

      .audio_l(sample_l),
      .audio_r(sample_r),

      .audio_lrck(audio_lrck),
      .audio_dac (audio_dac)
  );

endmodule
