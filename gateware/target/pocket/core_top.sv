//
// User core top-level
//
// Instantiated by the real top-level: apf_top
//

`default_nettype none

module core_top (

    //
    // physical connections
    //

    ///////////////////////////////////////////////////
    // clock inputs 74.25mhz. not phase aligned, so treat these domains as asynchronous

    input wire clk_74a,  // mainclk1
    input wire clk_74b,  // mainclk1

    ///////////////////////////////////////////////////
    // cartridge interface
    // switches between 3.3v and 5v mechanically
    // output enable for multibit translators controlled by pic32

    // GBA AD[15:8]
    inout  wire [7:0] cart_tran_bank2,
    output wire       cart_tran_bank2_dir,

    // GBA AD[7:0]
    inout  wire [7:0] cart_tran_bank3,
    output wire       cart_tran_bank3_dir,

    // GBA A[23:16]
    inout  wire [7:0] cart_tran_bank1,
    output wire       cart_tran_bank1_dir,

    // GBA [7] PHI#
    // GBA [6] WR#
    // GBA [5] RD#
    // GBA [4] CS1#/CS#
    //     [3:0] unwired
    inout  wire [7:4] cart_tran_bank0,
    output wire       cart_tran_bank0_dir,

    // GBA CS2#/RES#
    inout  wire cart_tran_pin30,
    output wire cart_tran_pin30_dir,
    // when GBC cart is inserted, this signal when low or weak will pull GBC /RES low with a special circuit
    // the goal is that when unconfigured, the FPGA weak pullups won't interfere.
    // thus, if GBC cart is inserted, FPGA must drive this high in order to let the level translators
    // and general IO drive this pin.
    output wire cart_pin30_pwroff_reset,

    // GBA IRQ/DRQ
    inout  wire cart_tran_pin31,
    output wire cart_tran_pin31_dir,

    // infrared
    input  wire port_ir_rx,
    output wire port_ir_tx,
    output wire port_ir_rx_disable,

    // GBA link port
    inout  wire port_tran_si,
    output wire port_tran_si_dir,
    inout  wire port_tran_so,
    output wire port_tran_so_dir,
    inout  wire port_tran_sck,
    output wire port_tran_sck_dir,
    inout  wire port_tran_sd,
    output wire port_tran_sd_dir,

    ///////////////////////////////////////////////////
    // cellular psram 0 and 1, two chips (64mbit x2 dual die per chip)

    output wire [21:16] cram0_a,
    inout  wire [ 15:0] cram0_dq,
    input  wire         cram0_wait,
    output wire         cram0_clk,
    output wire         cram0_adv_n,
    output wire         cram0_cre,
    output wire         cram0_ce0_n,
    output wire         cram0_ce1_n,
    output wire         cram0_oe_n,
    output wire         cram0_we_n,
    output wire         cram0_ub_n,
    output wire         cram0_lb_n,

    output wire [21:16] cram1_a,
    inout  wire [ 15:0] cram1_dq,
    input  wire         cram1_wait,
    output wire         cram1_clk,
    output wire         cram1_adv_n,
    output wire         cram1_cre,
    output wire         cram1_ce0_n,
    output wire         cram1_ce1_n,
    output wire         cram1_oe_n,
    output wire         cram1_we_n,
    output wire         cram1_ub_n,
    output wire         cram1_lb_n,

    ///////////////////////////////////////////////////
    // sdram, 512mbit 16bit

    output wire [12:0] dram_a,
    output wire [ 1:0] dram_ba,
    inout  wire [15:0] dram_dq,
    output wire [ 1:0] dram_dqm,
    output wire        dram_clk,
    output wire        dram_cke,
    output wire        dram_ras_n,
    output wire        dram_cas_n,
    output wire        dram_we_n,

    ///////////////////////////////////////////////////
    // sram, 1mbit 16bit

    output wire [16:0] sram_a,
    inout  wire [15:0] sram_dq,
    output wire        sram_oe_n,
    output wire        sram_we_n,
    output wire        sram_ub_n,
    output wire        sram_lb_n,

    ///////////////////////////////////////////////////
    // vblank driven by dock for sync in a certain mode

    input wire vblank,

    ///////////////////////////////////////////////////
    // i/o to 6515D breakout usb uart

    output wire dbg_tx,
    input  wire dbg_rx,

    ///////////////////////////////////////////////////
    // i/o pads near jtag connector user can solder to

    output wire user1,
    input  wire user2,

    ///////////////////////////////////////////////////
    // RFU internal i2c bus

    inout  wire aux_sda,
    output wire aux_scl,

    ///////////////////////////////////////////////////
    // RFU, do not use
    output wire vpll_feed,


    //
    // logical connections
    //

    ///////////////////////////////////////////////////
    // video, audio output to scaler
    output wire [23:0] video_rgb,
    output wire        video_rgb_clock,
    output wire        video_rgb_clock_90,
    output wire        video_de,
    output wire        video_skip,
    output wire        video_vs,
    output wire        video_hs,

    output wire audio_mclk,
    input  wire audio_adc,
    output wire audio_dac,
    output wire audio_lrck,

    ///////////////////////////////////////////////////
    // bridge bus connection
    // synchronous to clk_74a
    output wire        bridge_endian_little,
    input  wire [31:0] bridge_addr,
    input  wire        bridge_rd,
    output reg  [31:0] bridge_rd_data,
    input  wire        bridge_wr,
    input  wire [31:0] bridge_wr_data,

    ///////////////////////////////////////////////////
    // controller data
    //
    // key bitmap:
    //   [0]    dpad_up
    //   [1]    dpad_down
    //   [2]    dpad_left
    //   [3]    dpad_right
    //   [4]    face_a
    //   [5]    face_b
    //   [6]    face_x
    //   [7]    face_y
    //   [8]    trig_l1
    //   [9]    trig_r1
    //   [10]   trig_l2
    //   [11]   trig_r2
    //   [12]   trig_l3
    //   [13]   trig_r3
    //   [14]   face_select
    //   [15]   face_start
    //   [28:16] <unused>
    //   [31:29] type
    // joy values - unsigned
    //   [ 7: 0] lstick_x
    //   [15: 8] lstick_y
    //   [23:16] rstick_x
    //   [31:24] rstick_y
    // trigger values - unsigned
    //   [ 7: 0] ltrig
    //   [15: 8] rtrig
    //
    input wire [31:0] cont1_key,
    input wire [31:0] cont2_key,
    input wire [31:0] cont3_key,
    input wire [31:0] cont4_key,
    input wire [31:0] cont1_joy,
    input wire [31:0] cont2_joy,
    input wire [31:0] cont3_joy,
    input wire [31:0] cont4_joy,
    input wire [15:0] cont1_trig,
    input wire [15:0] cont2_trig,
    input wire [15:0] cont3_trig,
    input wire [15:0] cont4_trig,

    input  wire altera_reserved_tck,
    input  wire altera_reserved_tdi,
    output wire altera_reserved_tdo,
    input  wire altera_reserved_tms

);

  // not using the IR port, so turn off both the LED, and
  // disable the receive circuit to save power
  assign port_ir_tx              = 0;
  assign port_ir_rx_disable      = 1;

  // bridge endianness
  assign bridge_endian_little    = 0;

  // cart is unused, so set all level translators accordingly
  // directions are 0:IN, 1:OUT
  // assign cart_tran_bank3         = 8'hzz;
  // assign cart_tran_bank3_dir     = 1'b0;
  assign cart_tran_bank2         = 8'hzz;
  assign cart_tran_bank2_dir     = 1'b0;
  assign cart_tran_bank1         = 8'hzz;
  assign cart_tran_bank1_dir     = 1'b0;
  // assign cart_tran_bank0         = 4'hf;
  // assign cart_tran_bank0_dir     = 1'b1;
  assign cart_tran_pin30         = 1'b0;  // reset or cs2, we let the hw control it by itself
  assign cart_tran_pin30_dir     = 1'bz;
  assign cart_pin30_pwroff_reset = 1'b0;  // hardware can control this
  // assign cart_tran_pin31         = 1'bz;  // input
  // assign cart_tran_pin31_dir     = 1'b0;  // input

  // link port is input only
  assign port_tran_so            = 1'bz;
  assign port_tran_so_dir        = 1'b0;  // SO is output only
  assign port_tran_si            = 1'bz;
  assign port_tran_si_dir        = 1'b0;  // SI is input only
  assign port_tran_sck           = 1'bz;
  assign port_tran_sck_dir       = 1'b0;  // clock direction can change
  assign port_tran_sd            = 1'bz;
  assign port_tran_sd_dir        = 1'b0;  // SD is input and not used

  // tie off the rest of the pins we are not using
  assign cram0_a                 = 'h0;
  assign cram0_dq                = {16{1'bZ}};
  assign cram0_clk               = 0;
  assign cram0_adv_n             = 1;
  assign cram0_cre               = 0;
  assign cram0_ce0_n             = 1;
  assign cram0_ce1_n             = 1;
  assign cram0_oe_n              = 1;
  assign cram0_we_n              = 1;
  assign cram0_ub_n              = 1;
  assign cram0_lb_n              = 1;

  assign cram1_a                 = 'h0;
  assign cram1_dq                = {16{1'bZ}};
  assign cram1_clk               = 0;
  assign cram1_adv_n             = 1;
  assign cram1_cre               = 0;
  assign cram1_ce0_n             = 1;
  assign cram1_ce1_n             = 1;
  assign cram1_oe_n              = 1;
  assign cram1_we_n              = 1;
  assign cram1_ub_n              = 1;
  assign cram1_lb_n              = 1;

  // assign dram_a                  = 'h0;
  // assign dram_ba                 = 'h0;
  // assign dram_dq                 = {16{1'bZ}};
  // assign dram_dqm                = 'h0;
  // assign dram_clk                = 'h0;
  // assign dram_cke                = 'h0;
  // assign dram_ras_n              = 'h1;
  // assign dram_cas_n              = 'h1;
  // assign dram_we_n               = 'h1;

  assign sram_a                  = 'h0;
  assign sram_dq                 = {16{1'bZ}};
  assign sram_oe_n               = 1;
  assign sram_we_n               = 1;
  assign sram_ub_n               = 1;
  assign sram_lb_n               = 1;

  assign dbg_tx                  = 1'bZ;
  assign user1                   = 1'bZ;
  assign aux_scl                 = 1'bZ;
  assign vpll_feed               = 1'bZ;

  wire [31:0] apf_wishbone_bridge_rd_data;

  // for bridge write data, we just broadcast it to all bus devices
  // for bridge read data, we have to mux it
  // add your own devices here
  always @(*) begin
    casex (bridge_addr)
      default: begin
        bridge_rd_data <= 0;
      end
      32'h0XXX_XXXX: begin
        bridge_rd_data <= apf_wishbone_bridge_rd_data;
      end
      32'h1000_00XX: begin
        bridge_rd_data <= 0;
      end
      32'h1000_01XX: begin
        bridge_rd_data <= interact_read_data;
      end
      32'hF8XX_XXXX: begin
        bridge_rd_data <= cmd_bridge_rd_data;
      end
    endcase
  end

  always @(posedge clk_74a) begin
    if (menu_reset > 0) begin
      menu_reset <= menu_reset - 4'h1;
    end

    if (bridge_addr[31:28] == 4'h1 && bridge_wr) begin
      casex (bridge_addr[27:0])
        28'h0: begin
          menu_reset <= 4'hF;
        end
        28'h4: begin
          enable_reset_plus <= bridge_wr_data[0];
        end
        28'h8: begin
          use_jtag <= bridge_wr_data[0];
        end
        28'h1XX: begin
          // Directly in FIFO
        end
      endcase
    end
  end

  //
  // host/target command handler
  //
  wire reset_n;  // driven by host commands, can be used as core-wide reset
  wire [31:0] cmd_bridge_rd_data;

  // bridge host commands
  // synchronous to clk_74a
  wire status_boot_done = pll_core_locked_s74;
  wire status_setup_done = pll_core_locked_s74;  // rising edge triggers a target command
  wire status_running = reset_n;  // we are running as soon as reset_n goes high

  wire dataslot_requestread;
  wire [15:0] dataslot_requestread_id;
  wire dataslot_requestread_ack = 1;
  wire dataslot_requestread_ok = 1;

  wire dataslot_requestwrite;
  wire [15:0] dataslot_requestwrite_id;
  wire [31:0] dataslot_requestwrite_size;
  wire dataslot_requestwrite_ack = 1;
  wire dataslot_requestwrite_ok = 1;

  wire dataslot_update;
  wire [15:0] dataslot_update_id;
  wire [31:0] dataslot_update_size;

  wire dataslot_allcomplete;

  wire [31:0] rtc_epoch_seconds;
  wire [31:0] rtc_date_bcd;
  wire [31:0] rtc_time_bcd;
  wire rtc_valid;

  wire savestate_supported;
  wire [31:0] savestate_addr;
  wire [31:0] savestate_size;
  wire [31:0] savestate_maxloadsize;

  wire savestate_start;
  wire savestate_start_ack;
  wire savestate_start_busy;
  wire savestate_start_ok;
  wire savestate_start_err;

  wire savestate_load;
  wire savestate_load_ack;
  wire savestate_load_busy;
  wire savestate_load_ok;
  wire savestate_load_err;

  wire osnotify_inmenu;

  // bridge target commands
  // synchronous to clk_74a

  wire target_dataslot_request;
  wire [1:0] target_dataslot_request_type;

  wire target_dataslot_read = target_dataslot_request && target_dataslot_request_type == 2'h0;
  wire target_dataslot_write = target_dataslot_request && target_dataslot_request_type == 2'h1;
  wire target_dataslot_getfile = target_dataslot_request && target_dataslot_request_type == 2'h2;
  wire target_dataslot_openfile = target_dataslot_request && target_dataslot_request_type == 2'h3;

  wire target_dataslot_ack;
  wire target_dataslot_done;
  wire [2:0] target_dataslot_err;

  wire [15:0] target_dataslot_id;
  wire [31:0] target_dataslot_slotoffset;
  wire [15:0] taiko_bridge_offset_hi;  // Taiko Pocket: bits 47:32 of the read offset
  wire [31:0] target_dataslot_bridgeaddr;
  wire [31:0] target_dataslot_length;

  wire [31:0] target_buffer_param_struct = target_dataslot_bridgeaddr;
  wire [31:0] target_buffer_resp_struct = target_dataslot_bridgeaddr;

  // bridge data slot access
  // synchronous to clk_74a

  reg [9:0] datatable_addr;
  reg datatable_wren;
  reg [31:0] datatable_data;
  wire [31:0] datatable_q;

  core_bridge_cmd icb (

      .clk    (clk_74a),
      .reset_n(reset_n),

      .bridge_endian_little(bridge_endian_little),
      .bridge_addr         (bridge_addr),
      .bridge_rd           (bridge_rd),
      .bridge_rd_data      (cmd_bridge_rd_data),
      .bridge_wr           (bridge_wr),
      .bridge_wr_data      (bridge_wr_data),

      .status_boot_done (status_boot_done),
      .status_setup_done(status_setup_done),
      .status_running   (status_running),

      .dataslot_requestread    (dataslot_requestread),
      .dataslot_requestread_id (dataslot_requestread_id),
      .dataslot_requestread_ack(dataslot_requestread_ack),
      .dataslot_requestread_ok (dataslot_requestread_ok),

      .dataslot_requestwrite     (dataslot_requestwrite),
      .dataslot_requestwrite_id  (dataslot_requestwrite_id),
      .dataslot_requestwrite_size(dataslot_requestwrite_size),
      .dataslot_requestwrite_ack (dataslot_requestwrite_ack),
      .dataslot_requestwrite_ok  (dataslot_requestwrite_ok),

      .dataslot_update     (dataslot_update),
      .dataslot_update_id  (dataslot_update_id),
      .dataslot_update_size(dataslot_update_size),

      .dataslot_allcomplete(dataslot_allcomplete),

      .rtc_epoch_seconds(rtc_epoch_seconds),
      .rtc_date_bcd     (rtc_date_bcd),
      .rtc_time_bcd     (rtc_time_bcd),
      .rtc_valid        (rtc_valid),

      .savestate_supported  (savestate_supported),
      .savestate_addr       (savestate_addr),
      .savestate_size       (savestate_size),
      .savestate_maxloadsize(savestate_maxloadsize),

      .savestate_start     (savestate_start),
      .savestate_start_ack (savestate_start_ack),
      .savestate_start_busy(savestate_start_busy),
      .savestate_start_ok  (savestate_start_ok),
      .savestate_start_err (savestate_start_err),

      .savestate_load     (savestate_load),
      .savestate_load_ack (savestate_load_ack),
      .savestate_load_busy(savestate_load_busy),
      .savestate_load_ok  (savestate_load_ok),
      .savestate_load_err (savestate_load_err),

      .osnotify_inmenu(osnotify_inmenu),

      .target_dataslot_read    (target_dataslot_read),
      .target_dataslot_write   (target_dataslot_write),
      .target_dataslot_getfile (target_dataslot_getfile),
      .target_dataslot_openfile(target_dataslot_openfile),

      .target_dataslot_ack (target_dataslot_ack),
      .target_dataslot_done(target_dataslot_done),
      .target_dataslot_err (target_dataslot_err),

      .target_dataslot_id        (target_dataslot_id),
      .target_dataslot_slotoffset(target_dataslot_slotoffset),
      .target_dataslot_slotoffset_hi(taiko_bridge_offset_hi),
      .target_dataslot_bridgeaddr(target_dataslot_bridgeaddr),
      .target_dataslot_length    (target_dataslot_length),

      .target_buffer_param_struct(target_buffer_param_struct),
      .target_buffer_resp_struct (target_buffer_resp_struct),

      .datatable_addr(datatable_addr),
      .datatable_wren(datatable_wren),
      .datatable_data(datatable_data),
      .datatable_q   (datatable_q)

  );

  ////////////////////////////////////////////////////////////////////////////////////////
  // Interact CSR

  wire interact_region = bridge_addr >= 32'h1000_0100 && bridge_addr < 32'h1000_0200;

  wire interact_rd;
  wire interact_wr;
  wire [5:0] interact_addr = interact_wr ? interact_wr_addr : interact_rd_addr;
  wire [31:0] interact_data;

  wire [5:0] interact_rd_addr;
  wire [5:0] interact_wr_addr;

  wire [31:0] interact_q;

  wire [31:0] interact_read_data;

  sync_fifo #(
      .WIDTH(38)
  ) interact_write_sync_fifo (
      .clk_write(clk_74a),
      .clk_read (clk_sys_57_12),

      .write_en(interact_region && bridge_wr),
      .data({bridge_addr[7:2], bridge_wr_data}),

      .data_s({interact_wr_addr, interact_data}),
      .write_en_s(interact_wr)
  );

  sync_fifo #(
      .WIDTH(6)
  ) interact_read_addr_sync_fifo (
      .clk_write(clk_74a),
      .clk_read (clk_sys_57_12),

      .write_en(interact_region && bridge_rd),
      .data(bridge_addr[7:2]),

      .data_s(interact_rd_addr),
      .write_en_s(interact_rd)
  );

  reg interact_q_rd = 0;

  sync_fifo #(
      .WIDTH(32)
  ) interact_read_data_sync_fifo (
      .clk_write(clk_sys_57_12),
      .clk_read (clk_74a),

      .write_en(interact_q_rd),
      .data(interact_q),

      .data_s(interact_read_data)
      // .write_en_s()
  );

  reg [5:0] prev_interact_addr = 0;

  always @(posedge clk_sys_57_12) begin
    prev_interact_addr <= interact_addr;

    interact_q_rd <= 0;

    if (interact_addr != prev_interact_addr) begin
      // A cycle after we receive a new address, send the read data
      interact_q_rd <= 1;
    end
  end

  ////////////////////////////////////////////////////////////////////////////////////////
  // Dataslot Management

  reg [31:0] active_file_size = 0;
  reg prev_dataslot_update = 0;

  always @(posedge clk_74a) begin
    if (~pll_core_locked) begin
      datatable_addr <= 0;
      datatable_data <= 0;
      datatable_wren <= 0;
    end else begin
      prev_dataslot_update = dataslot_update;

      datatable_wren   <= 0;

      // Read asset size for data slot index set by ID
      // These are indicies, not IDs, so you must make sure the index matches the ID
      datatable_addr   <= target_dataslot_id[9:0] * 10'h2 + 10'h1;
      active_file_size <= datatable_q;

      if (apf_bridge_file_size_wr_s) begin
        // Set ID index
        datatable_addr <= target_dataslot_id[9:0] * 10'h2 + 10'h1;

        datatable_data <= apf_bridge_new_file_size_data_s;

        datatable_wren <= 1;
      end else if (dataslot_update && ~prev_dataslot_update) begin
        // Set ID index
        datatable_addr <= dataslot_update_id * 10'h2 + 10'h1;

        datatable_data <= dataslot_update_size;

        datatable_wren <= 1;
      end
    end
  end

  wire apf_bridge_file_size_wr;
  wire [31:0] apf_bridge_new_file_size_data;

  wire apf_bridge_file_size_wr_s;
  wire [31:0] apf_bridge_new_file_size_data_s;

  sync_fifo #(
      .WIDTH(32)
  ) dataslot_update_sync_fifo (
      .clk_write(clk_sys_57_12),
      .clk_read (clk_74a),

      .write_en(apf_bridge_file_size_wr),
      .data(apf_bridge_new_file_size_data),

      .data_s(apf_bridge_new_file_size_data_s),
      .write_en_s(apf_bridge_file_size_wr_s)
  );

  ////////////////////////////////////////////////////////////////////////////////////////
  // Data loading

  reg ioctl_download;

  always @(posedge clk_74a) begin
    if (dataslot_requestwrite) ioctl_download <= 1;
    else if (dataslot_allcomplete) ioctl_download <= 0;
  end


  ////////////////////////////////////////////////////////////////////////////////////////
  // Debug UART

  wire reload_rom_active;
  wire uart_rx;
  wire uart_tx;

  debug_key debug_key (
      .clk(clk_sys_57_12),

      .cart_tran_bank0_dir(cart_tran_bank0_dir),
      .cart_tran_bank0(cart_tran_bank0),

      .cart_tran_bank3_dir(cart_tran_bank3_dir),
      .cart_tran_bank3(cart_tran_bank3),

      .cart_tran_pin31_dir(cart_tran_pin31_dir),
      .cart_tran_pin31(cart_tran_pin31),

      // IO
      .led(reload_rom_active),
      // .button(cart_button),

      .uart_rx(uart_rx),
      .uart_tx(uart_tx)
  );

  ////////////////////////////////////////////////////////////////////////////////////////
  // Chip ID

  wire [63:0] chip_id;

  chip_id chip_id_controller (
      .clkin  (clk_sys_57_12),
      .reset  (~reset_n_s),
      .chip_id(chip_id)
  );

  ////////////////////////////////////////////////////////////////////////////////////////
  // Settings and Sync

  wire reset_input_button_s;

  synch_3 reset_button_synch (
      cont1_key[15],
      reset_input_button_s,
      clk_sys_57_12
  );

  reg [3:0] menu_reset = 0;
  reg enable_reset_plus = 0;
  reg use_jtag = 0;

  wire reset_n_s;
  wire menu_reset_s;
  wire enable_reset_plus_s;
  wire use_jtag_s;

  synch_3 #(
      .WIDTH(4)
  ) settings_synch (
      {reset_n, menu_reset == 4'h1, enable_reset_plus, use_jtag},
      {reset_n_s, menu_reset_s, enable_reset_plus_s, use_jtag_s},
      clk_sys_57_12
  );

  wire rtc_valid_s;
  wire [31:0] rtc_epoch_seconds_s;
  wire [31:0] rtc_date_bcd_s;
  wire [31:0] rtc_time_bcd_s;

  // These values won't change, so normal multibit synchs are fine
  synch_3 #(
      .WIDTH(97)
  ) rtc_synch (
      {rtc_valid, rtc_epoch_seconds, rtc_date_bcd, rtc_time_bcd},
      {rtc_valid_s, rtc_epoch_seconds_s, rtc_date_bcd_s, rtc_time_bcd_s},
      clk_sys_57_12
  );

  reg [31:0] rtc_counter = 0;
  reg [31:0] rtc_second_counter = 0;

  reg prev_rtc_valid = 0;

  always @(posedge clk_sys_57_12) begin
    prev_rtc_valid <= rtc_valid_s;

    if (rtc_second_counter > 0) begin
      rtc_second_counter <= rtc_second_counter - 32'h1;
    end else begin
      rtc_second_counter <= 57_120_000;
      rtc_counter <= rtc_counter + 32'h1;
    end

    if (rtc_valid_s && ~prev_rtc_valid) begin
      rtc_counter <= rtc_epoch_seconds_s;
      rtc_second_counter <= 57_120_000;
    end
  end

  reg [15:0] reset_timer = 0;

  reg prev_reset_button = 0;

  wire reset_button = menu_reset_s || (reset_input_button_s && enable_reset_plus_s);

  always @(posedge clk_sys_57_12) begin
    prev_reset_button <= reset_button;

    if (reset_button && ~prev_reset_button) begin
      reset_timer = 16'hFFFF;
    end else if (reset_timer > 0) begin
      reset_timer <= reset_timer - 16'h1;
    end
  end

  wire apf_bridge_request_read;
  wire apf_bridge_request_write;
  wire apf_bridge_request_getfile;
  wire apf_bridge_request_openfile;

  wire [31:0] apf_bridge_data_offset;
  wire [31:0] apf_bridge_length;
  wire [15:0] apf_bridge_slot_id;

  reg [2:0] apf_bridge_request_counter = 0;
  reg [1:0] apf_bridge_request_type = 0;

  reg prev_apf_bridge_request_read = 0;
  reg prev_apf_bridge_request_write = 0;
  reg prev_apf_bridge_request_getfile = 0;
  reg prev_apf_bridge_request_openfile = 0;

  // Signal lengthener for faster bridge clock domain
  always @(posedge clk_sys_57_12) begin
    prev_apf_bridge_request_read <= apf_bridge_request_read;
    prev_apf_bridge_request_write <= apf_bridge_request_write;
    prev_apf_bridge_request_getfile <= apf_bridge_request_getfile;
    prev_apf_bridge_request_openfile <= apf_bridge_request_openfile;

    if (apf_bridge_request_read && ~prev_apf_bridge_request_read) begin
      apf_bridge_request_counter <= 3'h7;
      apf_bridge_request_type <= 2'h0;
    end else if (apf_bridge_request_write && ~prev_apf_bridge_request_write) begin
      apf_bridge_request_counter <= 3'h7;
      apf_bridge_request_type <= 2'h1;
    end else if (apf_bridge_request_getfile && ~prev_apf_bridge_request_getfile) begin
      apf_bridge_request_counter <= 3'h7;
      apf_bridge_request_type <= 2'h2;
    end else if (apf_bridge_request_openfile && ~prev_apf_bridge_request_openfile) begin
      apf_bridge_request_counter <= 3'h7;
      apf_bridge_request_type <= 2'h3;
    end

    if (apf_bridge_request_counter > 3'h0) begin
      apf_bridge_request_counter <= apf_bridge_request_counter - 3'h1;
    end
  end

  wire apf_bridge_request = apf_bridge_request_counter > 3'h0;

  wire [31:0] active_file_size_s;
  wire target_dataslot_done_s;

  synch_3 #(
      .WIDTH(33)
  ) from_bridge_s (
      {active_file_size, target_dataslot_done},
      {active_file_size_s, target_dataslot_done_s},
      clk_sys_57_12
  );

  // Always have bridge read/write from 0
  assign target_dataslot_bridgeaddr = 32'h0;

  synch_3 #(
      .WIDTH(83)
  ) to_bridge_s (
      {
        apf_bridge_request,
        apf_bridge_request_type,
        apf_bridge_data_offset,
        apf_bridge_length,
        apf_bridge_slot_id
      },
      {
        target_dataslot_request,
        target_dataslot_request_type,
        target_dataslot_slotoffset,
        target_dataslot_length,
        target_dataslot_id
      },
      clk_74a
  );

  ////////////////////////////////////////////////////////////////////////////////////////
  // Core

  wire reset = ~reset_n_s || ioctl_download || reset_timer > 0;

  // wire ack;
  // wire [29:0] addr;
  // wire [1:0] bte;
  // wire [2:0] cti;
  // wire cyc;
  // wire [31:0] data_read;
  // wire [31:0] data_write;
  // wire err;
  // wire [3:0] sel;
  // wire stb;
  // wire we;

  wire apf_master_ack;
  wire [29:0] apf_master_addr;
  wire [1:0] apf_master_bte;
  wire [2:0] apf_master_cti;
  wire apf_master_cyc;
  wire [31:0] apf_master_data_read;
  wire [31:0] apf_master_data_write;
  wire apf_master_err;
  wire [3:0] apf_master_sel;
  wire apf_master_stb;
  wire apf_master_we;

  // Taiko Pocket (gateware/taiko)
  wire [29:0] taiko_wbs_adr;
  wire [31:0] taiko_wbs_dat_w;
  wire [31:0] taiko_wbs_dat_r;
  wire taiko_wbs_cyc;
  wire taiko_wbs_stb;
  wire taiko_wbs_we;
  wire taiko_wbs_ack;
  wire [29:0] taiko_wbm_adr;
  wire taiko_wbm_cyc;
  wire taiko_wbm_stb;
  wire bus_master_ack;
  wire [31:0] bus_master_data_read;
  wire bus_master_err;
  reg stream_grant = 0;

  wire [31:0] ram_data_address;
  reg [31:0] latched_ram_data_address = 0;
  wire [25:0] current_address;

  wire [31:0] audio_bus_out;
  wire audio_bus_wr;
  wire audio_playback_en;
  wire audio_flush;
  wire [11:0] audio_buffer_fill;

  reg prev_target_dataslot_done_s = 0;

  always @(posedge clk_sys_57_12) begin
    prev_target_dataslot_done_s <= target_dataslot_done_s;

    if (apf_bridge_request_read || apf_bridge_request_write || apf_bridge_request_getfile || apf_bridge_request_openfile) begin
      latched_ram_data_address <= ram_data_address;
    end
  end

  litex litex (
      .clk_sys(clk_sys_57_12),
      .clk_sys2x(clk_mem_114_24),
      .clk_sys2x_90deg(clk_mem_114_24_90deg),
      .clk_vid(clk_vid),

      .reset(reset),

      .apf_audio_bus_out(audio_bus_out),
      .apf_audio_bus_wr(audio_bus_wr),
      .apf_audio_playback_en(audio_playback_en),
      .apf_audio_flush(audio_flush),
      .apf_audio_buffer_fill(audio_buffer_fill),

      .apf_bridge_request_read(apf_bridge_request_read),
      .apf_bridge_request_write(apf_bridge_request_write),
      .apf_bridge_request_getfile(apf_bridge_request_getfile),
      .apf_bridge_request_openfile(apf_bridge_request_openfile),

      .apf_bridge_data_offset(apf_bridge_data_offset),
      .apf_bridge_length(apf_bridge_length),
      .apf_bridge_ram_data_address(ram_data_address),
      .apf_bridge_slot_id(apf_bridge_slot_id),
      .apf_bridge_file_size(active_file_size_s),
      .apf_bridge_file_size_wr(apf_bridge_file_size_wr),
      .apf_bridge_new_file_size_data(apf_bridge_new_file_size_data),
      .apf_bridge_current_address(current_address),
      // Pulse complete on rising edge of done
      .apf_bridge_complete_trigger(target_dataslot_done_s && ~prev_target_dataslot_done_s),
      .apf_bridge_command_result_code(target_dataslot_err),

      .apf_id_chip_id(chip_id),

      .apf_input_cont1_key (cont1_key),
      .apf_input_cont1_joy (cont1_joy),
      .apf_input_cont1_trig(cont1_trig),

      .apf_input_cont2_key (cont2_key),
      .apf_input_cont2_joy (cont2_joy),
      .apf_input_cont2_trig(cont2_trig),

      .apf_input_cont3_key (cont3_key),
      .apf_input_cont3_joy (cont3_joy),
      .apf_input_cont3_trig(cont3_trig),

      .apf_input_cont4_key (cont4_key),
      .apf_input_cont4_joy (cont4_joy),
      .apf_input_cont4_trig(cont4_trig),

      .apf_interact_address(interact_addr),
      .apf_interact_data(interact_data),
      .apf_interact_q(interact_q),
      .apf_interact_wr(interact_wr),

      .apf_rtc_date_bcd(rtc_date_bcd_s),
      .apf_rtc_time_bcd(rtc_time_bcd_s),
      .apf_rtc_unix_seconds(rtc_counter),

      // Taiko Pocket registers: the SoC's external Wishbone region at 0x8000_0000
      .wishbone_ack(taiko_wbs_ack),
      .wishbone_adr(taiko_wbs_adr),
      .wishbone_bte(),
      .wishbone_cti(),
      .wishbone_cyc(taiko_wbs_cyc),
      .wishbone_dat_r(taiko_wbs_dat_r),
      .wishbone_dat_w(taiko_wbs_dat_w),
      .wishbone_err(1'b0),
      .wishbone_sel(),
      .wishbone_stb(taiko_wbs_stb),
      .wishbone_we(taiko_wbs_we),

      // bus master port, shared by the APF bridge (file data) and the music streamer
      .wishbone_master_ack(bus_master_ack),
      .wishbone_master_adr(stream_grant ? taiko_wbm_adr : apf_master_addr),
      .wishbone_master_bte(stream_grant ? 2'b0 : apf_master_bte),
      .wishbone_master_cti(stream_grant ? 3'b0 : apf_master_cti),
      .wishbone_master_cyc(stream_grant ? taiko_wbm_cyc : apf_master_cyc),
      .wishbone_master_dat_r(bus_master_data_read),
      .wishbone_master_dat_w(apf_master_data_write),
      .wishbone_master_err(bus_master_err),
      .wishbone_master_sel(stream_grant ? 4'hF : apf_master_sel),
      .wishbone_master_stb(stream_grant ? taiko_wbm_stb : apf_master_stb),
      .wishbone_master_we(stream_grant ? 1'b0 : apf_master_we),

      .vga_r(rgb565[15:11]),
      .vga_g(rgb565[10:5]),
      .vga_b(rgb565[4:0]),
      .vga_hsync(hsync),
      .vga_vsync(vsync),
      .vga_de(de),

      .use_jtag(use_jtag_s),

      // Altera JTAG UART
      .altera_reserved_tck(altera_reserved_tck),
      .altera_reserved_tdi(altera_reserved_tdi),
      .altera_reserved_tdo(altera_reserved_tdo),
      .altera_reserved_tms(altera_reserved_tms),

      // Dev cart UART
      .serial_rx(uart_rx),
      .serial_tx(uart_tx),

      .sdram_a(dram_a),
      .sdram_ba(dram_ba),
      .sdram_cas_n(dram_cas_n),
      .sdram_cke(dram_cke),
      .sdram_clock(dram_clk),
      .sdram_dm(dram_dqm),
      .sdram_dq(dram_dq),
      .sdram_ras_n(dram_ras_n),
      .sdram_we_n(dram_we_n)
  );

  // wishbone wishbone (
  //     .clk(clk_sys_57_12),

  //     .reset(reset),

  //     .addr(addr),
  //     .bte(bte),
  //     .cti(cti),
  //     .cyc(cyc),
  //     .data_write(data_write),
  //     .data_read(data_read),
  //     .sel(sel),
  //     .stb(stb),
  //     .we(we),
  //     .ack(ack),
  //     .err(err)
  // );

  apf_wishbone_master apf_wishbone_master (
      .clk_74a(clk_74a),
      .clk_sys(clk_sys_57_12),

      .bridge_addr(bridge_addr),
      .bridge_wr_data(bridge_wr_data),
      .bridge_rd_data(apf_wishbone_bridge_rd_data),
      .bridge_wr(bridge_wr),
      .bridge_rd(bridge_rd),
      .bridge_endian_little(bridge_endian_little),

      // Write to start of SDRAM when uploading data
      .ram_data_address(latched_ram_data_address),

      .current_address(current_address),

      .addr(apf_master_addr),
      .bte(apf_master_bte),
      .cti(apf_master_cti),
      .cyc(apf_master_cyc),
      .data_write(apf_master_data_write),
      .data_read(apf_master_data_read),
      .sel(apf_master_sel),
      .stb(apf_master_stb),
      .we(apf_master_we),
      .ack(apf_master_ack),
      .err(apf_master_err)
  );

  ////////////////////////////////////////////////////////////////////////////////////////
  // Video

  wire vsync;
  wire hsync;
  wire de;
  wire [15:0] rgb565;
  wire [23:0] rgb888;

  rgb565_to_rgb888 rgb565_to_rgb888 (
      .rgb565(rgb565),
      .rgb888(rgb888)
  );

  reg [23:0] rgb_delay = 0;

  reg de_delay = 0;
  reg de_delay2 = 0;
  reg vs_delay = 0;
  reg hs_delay = 0;

  // Taiko Pocket: the picture passes through the sprite layer first
  wire [23:0] ovl_rgb;
  wire ovl_de, ovl_hs, ovl_vs;

  always @(posedge clk_vid) begin
    rgb_delay <= ovl_rgb;

    de_delay  <= ovl_de; // period when rgb_delay is valid
    de_delay2 <= de_delay;
    vs_delay  <= ovl_vs;
    hs_delay  <= ovl_hs;
  end

  reg [3:0] de_counter = 0;

  assign video_rgb_clock = clk_vid;
  assign video_rgb_clock_90 = clk_vid_90deg;
  // Taiko Pocket: until the game has set up the picture, the core sends black frames of
  // its own, with the timing the game will use (400 x 360 in 476 x 400, as in video.json).
  // Without frames the Pocket's scaler shows what is left in its buffer: stripes after
  // power-on, the last picture of the previous session otherwise. The game's picture
  // takes over at the start of one of its frames.
  wire taiko_video_on;
  reg [1:0] video_on_s = 0;
  reg [8:0] boot_h = 0, boot_v = 0;
  reg show_game = 0, vs_before = 0;
  reg [23:0] out_rgb = 0;
  reg out_de = 0, out_vs = 0, out_hs = 0;
  always @(posedge clk_vid) begin
    video_on_s <= {video_on_s[0], taiko_video_on};
    if (boot_h == 9'd475) begin
      boot_h <= 0;
      boot_v <= (boot_v == 9'd399) ? 9'd0 : boot_v + 9'd1;
    end else begin
      boot_h <= boot_h + 9'd1;
    end
    vs_before <= vs_delay;
    if (!video_on_s[1]) show_game <= 0;
    else if (vs_delay && !vs_before) show_game <= 1;

    out_rgb <= (show_game && de_delay) ? rgb_delay : 24'h0;
    out_de  <= show_game ? de_delay : (boot_h < 9'd400 && boot_v < 9'd360);
    out_hs  <= show_game ? hs_delay : (boot_h >= 9'd408 && boot_h < 9'd440);
    out_vs  <= show_game ? vs_delay : (boot_v >= 9'd361 && boot_v < 9'd370);
  end
  assign video_rgb = out_rgb;
  // Taiko Pocket: exactly the active pixels (the stock core adds a black column on each
  // side, which would break the integer 4x scale of 400 pixels)
  assign video_de = out_de;
  assign video_skip = 0;
  assign video_vs = out_vs;
  assign video_hs = out_hs;

  ////////////////////////////////////////////////////////////////////////////////////////
  // Sound

  wire [31:0] audio_fifo_wr_data;
  wire audio_fifo_wrreq;
  wire audio_fifo_wrfull;
  wire [31:0] audio_fifo_q;
  wire audio_fifo_rdempty;
  wire audio_fifo_rdreq;
  wire audio_tick;
  wire [15:0] audio_sample_l;
  wire [15:0] audio_sample_r;

  audio audio (
      .clk_74b(clk_74b),
      .clk_sys(clk_sys_57_12),

      .reset(reset),

      .fifo_wr_data(audio_fifo_wr_data),
      .fifo_wrreq(audio_fifo_wrreq),
      .fifo_wrfull(audio_fifo_wrfull),
      .audio_flush(audio_flush),
      .audio_buffer_fill(audio_buffer_fill),

      .fifo_q(audio_fifo_q),
      .fifo_rdempty(audio_fifo_rdempty),
      .fifo_rdreq(audio_fifo_rdreq),
      .tick(audio_tick),
      .sample_l(audio_sample_l),
      .sample_r(audio_sample_r),

      .audio_mclk(audio_mclk),
      .audio_lrck(audio_lrck),
      .audio_dac (audio_dac)
  );

  ////////////////////////////////////////////////////////////////////////////////////////
  // Taiko Pocket: hit sounds, input timestamps, music streamer (gateware/taiko)

  // The streamer gets the bus master port whenever the bridge is not using it.
  always @(posedge clk_sys_57_12) begin
    if (!stream_grant && !apf_master_cyc && taiko_wbm_cyc) stream_grant <= 1;
    else if (stream_grant && !taiko_wbm_cyc) stream_grant <= 0;
  end
  assign apf_master_ack = bus_master_ack && !stream_grant;
  assign apf_master_data_read = bus_master_data_read;
  assign apf_master_err = bus_master_err && !stream_grant;

  // any controller's buttons play the drum: the Pocket's own, or the Dock's
  wire [15:0] taiko_keys = cont1_key[15:0] | cont2_key[15:0] | cont3_key[15:0] | cont4_key[15:0];

  taiko_top taiko (
      .clk_sys(clk_sys_57_12),
      .rst(reset),

      .wbs_adr(taiko_wbs_adr),
      .wbs_dat_w(taiko_wbs_dat_w),
      .wbs_dat_r(taiko_wbs_dat_r),
      .wbs_cyc(taiko_wbs_cyc),
      .wbs_stb(taiko_wbs_stb),
      .wbs_we(taiko_wbs_we),
      .wbs_ack(taiko_wbs_ack),

      .wbm_adr(taiko_wbm_adr),
      .wbm_cyc(taiko_wbm_cyc),
      .wbm_stb(taiko_wbm_stb),
      .wbm_ack(bus_master_ack && stream_grant),
      .wbm_dat_r(bus_master_data_read),

      .cpu_audio_data(audio_bus_out),
      .cpu_audio_wr(audio_bus_wr),
      .cpu_playback_en(audio_playback_en),

      .fifo_wr_data(audio_fifo_wr_data),
      .fifo_wrreq(audio_fifo_wrreq),
      .fifo_wrfull(audio_fifo_wrfull),

      .clk_audio(audio_mclk),
      .tick(audio_tick),
      .fifo_q(audio_fifo_q),
      .fifo_rdempty(audio_fifo_rdempty),
      .fifo_rdreq(audio_fifo_rdreq),
      .out_l(audio_sample_l),
      .out_r(audio_sample_r),

      .keys_async(taiko_keys),
      .vblank_async(vs_delay),

      .bridge_offset_hi(taiko_bridge_offset_hi),
      .video_on(taiko_video_on),
      .clk_vid(clk_vid),
      .vid_rgb_in(rgb888),
      .vid_de_in(de),
      .vid_hs_in(hsync),
      .vid_vs_in(vsync),
      .vid_rgb_out(ovl_rgb),
      .vid_de_out(ovl_de),
      .vid_hs_out(ovl_hs),
      .vid_vs_out(ovl_vs)
  );


  ////////////////////////////////////////////////////////////////////////////////////////
  // PLL

  wire clk_sys_57_12;
  wire clk_mem_114_24;
  wire clk_mem_114_24_90deg;
  wire clk_vid;
  wire clk_vid_90deg;

  wire pll_core_locked;
  wire pll_core_locked_s74;
  synch_3 s01 (
      pll_core_locked,
      pll_core_locked_s74,
      clk_74a
  );

  mf_pllbase mp1 (
      .refclk(clk_74a),
      .rst   (0),

      .outclk_0(clk_sys_57_12),
      .outclk_1(clk_mem_114_24),
      .outclk_2(clk_mem_114_24_90deg),
      .outclk_3(clk_vid),
      .outclk_4(clk_vid_90deg),

      .locked(pll_core_locked)
  );

endmodule
