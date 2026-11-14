# gateware

The hardware side of Taiko Pocket: an FPGA design for the Analogue Pocket.

- `taiko/`: what this project adds (hit sounds, input timestamps, music streaming,
  the sprite layer) and its test bench. Start with [taiko/README.md](taiko/README.md).
- Everything else is the base it is built on:
  [openfpga-litex](https://github.com/agg23/openfpga-litex) by agg23 (MIT, see
  `LICENSE`), a LiteX system with a RISC-V CPU for the Pocket, taken at commit
  `c21570d`. `target/pocket/core_top.sv` and a few files next to it are changed to
  connect `taiko/`; the example programs and Rust crates of the original are not here.
- `pkg/pocket/`: the core's definition files as they go onto the SD card.
- `docs/control.md`: the base system's registers, from the original.

How the core is built: [docs/pocket.md](../docs/pocket.md).
