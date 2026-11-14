# Taiko gateware: lint and simulation with Verilator. Output goes to build/gateware at the repo root.
ROOT := $(abspath ../..)
OUT := $(ROOT)/build/gateware
RTL := rtl/taiko_cdc.sv rtl/taiko_audio.sv rtl/taiko_stream.sv rtl/taiko_sprites.sv rtl/taiko_top.sv
# PROCASSINIT (registers with power-up values) only exists in newer Verilator releases
PROCASSINIT := $(shell verilator -Wno-PROCASSINIT --version >/dev/null 2>&1 && echo -Wno-PROCASSINIT)
WARN := -Wall -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM -Wno-PINCONNECTEMPTY -Wno-MULTITOP $(PROCASSINIT)

.PHONY: sim lint
# The test bench links the game's software audio engine and sprite layer (C) as the reference models.
sim: lint
	@mkdir -p $(OUT)/obj
	$(CC) -O1 -std=c11 -I$(ROOT)/src -c -o $(OUT)/softmix.o $(ROOT)/src/platform/common/softmix.c
	$(CC) -O1 -std=c11 -I$(ROOT)/src -c -o $(OUT)/adpcm.o $(ROOT)/src/core/adpcm.c
	$(CC) -O1 -std=c11 -I$(ROOT)/src -c -o $(OUT)/softovl.o $(ROOT)/src/platform/common/softovl.c
	verilator --cc --exe --build -j 0 $(WARN) -Wno-fatal --public-flat-rw --top-module taiko_top \
		--Mdir $(OUT)/obj -o ../tb_taiko -CFLAGS "-std=c++17 -O1 -I$(ROOT)/src" \
		$(RTL) tb/tb_taiko.cpp $(OUT)/softmix.o $(OUT)/adpcm.o $(OUT)/softovl.o > $(OUT)/verilator.log 2>&1 || (tail -30 $(OUT)/verilator.log; false)
	$(OUT)/tb_taiko

lint:
	verilator --lint-only $(WARN) --top-module taiko_top $(RTL)
