# Platform facts (brief section 2), as verified

| Claim in the brief | Finding | How |
|---|---|---|
| CPU clock under about 60 MHz | The stock core runs at 57.12 MHz; kept. | `gateware/litex/analogue_pocket.py`, PLL settings |
| Video at most 800x720 | Confirmed. Pixel clock about 1 to 50 MHz, 47 to 61 Hz. | Analogue developer docs |
| 4,096-sample audio buffer, 85 ms latency | It is a FIFO the CPU fills; the latency is whatever is queued, up to 85 ms. Samples leave it at 48 kHz in the 12.288 MHz audio clock domain, so a mixer after it adds none. | `target/pocket/audio.sv` |
| File API cannot open paths | **It can**: bridge command `0x0192` (open a file by path into a slot) is wired up in openfpga-litex. The single-library design does not depend on it. | `docs/control.md` of openfpga-litex, Analogue docs |
| Maximum file size / 32-bit offsets | The stock core only sends the 32-bit read (`0x0180`): 4 GB. APF also has a 48-bit read (`0x0181`); this core now sends it when needed. Lengths are 32-bit. Untested on hardware. | `core_bridge_cmd.v`, Analogue docs |
| SD writes are broken | Unverified. The save file is created with `0x0192` and written with `0x0184`; a failure turns saving off for the session (`docs/pocket.md`). | needs hardware |
| Input update rate | The APF pad controller in the core polls every 2^16 cycles of 74.25 MHz: **every 0.88 ms**, plus about 0.3 ms for the reply. Worst-case age of a button state about 1.2 ms; jitter 0.9 ms (43 samples). | `platform/pocket/io_pad_controller.v` |
| Block RAM: 31% used | With this core's audio and sprite memories: 284 of 308 M10K blocks (92 %), `docs/performance.md`. | fit report |

Other findings:

- The SoC's video timing generator and frame buffer are programmable at run time,
  and the stock netlist already has an unused external Wishbone port and a
  bus-master port. So 400x360 and all Taiko gateware were added **without
  regenerating the LiteX SoC**; only the pixel-clock PLL output changed.
- The stock core adds one black column left and right of the picture. Removed, or
  400 pixels would not scale by exactly 4.
- The SoC's own vertical-blank register compares against 240 lines; the Taiko
  register block has its own frame counter.

See `gateware/taiko/README.md` for the latency budget.
