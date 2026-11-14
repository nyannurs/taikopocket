# Taiko Pocket: desktop build, tests, packer helpers.
#   make            desktop game (build/taikopocket)
#   make test       unit tests, the game on a simulated platform, packer tests
#   make golden     parser and judge against YataiDON's own code (needs third_party/)
#   make run        pack the test songs and start the game
#   make web        browser build (needs emscripten)     -> build/web/
#   make pocket     RISC-V firmware (needs riscv gcc)    -> build/pocket/
#   make sim        gateware test bench (needs verilator)
# Everything generated goes to build/.

BUILD ?= build
CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter -Isrc
SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
SDL_LIBS := $(shell sdl2-config --libs 2>/dev/null)
# `make venv` creates build/venv with Pillow; it is used when present
PYTHON ?= $(if $(wildcard $(BUILD)/venv/bin/python),$(BUILD)/venv/bin/python,python3)

CORE_SRC := $(wildcard src/core/*.c)
GAME_SRC := $(wildcard src/game/*.c)
COMMON_SRC := $(wildcard src/platform/common/*.c)
SDL_SRC := $(wildcard src/platform/sdl/*.c)

APP_OBJ := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(CORE_SRC) $(GAME_SRC) $(COMMON_SRC) $(SDL_SRC))
CORE_OBJ := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(CORE_SRC))

TEST_SRC := $(wildcard tests/test_*.c)
TEST_BIN := $(patsubst tests/%.c,$(BUILD)/tests/%,$(TEST_SRC))

.PHONY: all test run clean web pocket sim golden venv
.DEFAULT_GOAL := all
venv:
	python3 -m venv $(BUILD)/venv
	$(BUILD)/venv/bin/pip install --quiet --disable-pip-version-check pillow
all: $(BUILD)/taikopocket

$(BUILD)/taikopocket: $(APP_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(SDL_LIBS) -lm

$(BUILD)/obj/platform/sdl/%.o: src/platform/sdl/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -MMD -c -o $@ $<

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -c -o $@ $<

$(BUILD)/tests/%: tests/%.c tests/test.h $(CORE_OBJ) $(BUILD)/obj/platform/common/softmix.o
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-missing-field-initializers -Itests -o $@ $(filter-out %.h,$^) -lm

HOST_SRC := tests/host/host_platform.c tests/host/host.h tests/test.h
GAME_OBJ := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(GAME_SRC) $(COMMON_SRC))

$(BUILD)/tplist: tools/tplist.c $(HOST_SRC) $(CORE_OBJ) $(GAME_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests -o $@ $(filter-out %.h,$^) -lm

$(BUILD)/tplcheck: tools/tplcheck.c $(HOST_SRC) $(CORE_OBJ) $(GAME_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests -o $@ $(filter-out %.h,$^) -lm

# the whole game on the simulated platform
$(BUILD)/tests/test_game: tests/game/test_game.c $(HOST_SRC) $(CORE_OBJ) $(GAME_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests -o $@ $(filter-out %.h,$^) -lm

# The in-repo test charts, packed without audio, with the default skin.
$(BUILD)/tests/songs.tpl: $(wildcard tests/data/songs/*/*) $(wildcard tools/packer/*.py tools/packer/taikopack/*.py) src/game/assets.def \
		$(wildcard skins/taikojiro-original/*/*)
	@mkdir -p $(dir $@)
	$(PYTHON) tools/packer/pack.py tests/data/songs -o $@ --audio none > $(BUILD)/tests/pack.log

test: $(TEST_BIN) $(BUILD)/tplist $(BUILD)/tests/songs.tpl $(BUILD)/tests/test_game
	@set -e; for t in $(TEST_BIN); do echo "== $$t"; $$t; done
	@echo "== $(BUILD)/tests/test_game"; $(BUILD)/tests/test_game $(BUILD)/tests/songs.tpl
	$(PYTHON) -m unittest discover -s tools/packer/tests -t tools/packer -q
	@# the C loader and the Python reader must agree on the library's contents
	$(PYTHON) tools/packer/pack.py --list $(BUILD)/tests/songs.tpl | tail -n +2 > $(BUILD)/tests/list_py.txt
	$(BUILD)/tplist $(BUILD)/tests/songs.tpl > $(BUILD)/tests/list_c.txt
	cmp $(BUILD)/tests/list_py.txt $(BUILD)/tests/list_c.txt
	@echo "all tests passed"

# Browser build (Emscripten). Its download cache stays inside build/.
WEB_SRC := $(CORE_SRC) $(GAME_SRC) $(COMMON_SRC) $(SDL_SRC)
web: $(BUILD)/web/index.html
$(BUILD)/web/index.html: $(WEB_SRC) web/shell.html $(wildcard src/*/*.h src/*/*/*.h src/game/assets.def)
	@mkdir -p $(BUILD)/web $(BUILD)/emcache
	EM_CACHE=$(abspath $(BUILD))/emcache emcc -O2 -std=c11 -Isrc -sUSE_SDL=2 \
		-sINITIAL_MEMORY=128MB -sSTACK_SIZE=1MB -sENVIRONMENT=web \
		-sEXPORTED_FUNCTIONS=_main,_web_read_done,_web_music_pos,_web_mixer_counter,_web_callback_frames -sEXPORTED_RUNTIME_METHODS=callMain \
		--shell-file web/shell.html -o $@ $(WEB_SRC)

# Pocket firmware (RISC-V, freestanding). Needs riscv64-elf-gcc or riscv64-unknown-elf-gcc.
RISCV ?= $(shell command -v riscv64-elf-gcc >/dev/null 2>&1 && echo riscv64-elf- || echo riscv64-unknown-elf-)
POCKET_SRC := $(CORE_SRC) $(GAME_SRC) src/platform/pocket/platform_pocket.c src/platform/pocket/libc_min.c
POCKET_FLAGS := -march=rv32imafdc -mabi=ilp32d -O2 -std=c11 -ffreestanding -fno-builtin-printf -fno-tree-loop-distribute-patterns -fdata-sections \
	-ffunction-sections -Wall -Wextra -Wno-unused-parameter -Isrc/platform/pocket/include -Isrc
pocket: $(BUILD)/pocket/taikopocket.bin
$(BUILD)/pocket/taikopocket.bin: $(POCKET_SRC) src/platform/pocket/crt0.S src/platform/pocket/link.ld $(wildcard src/*/*.h src/*/*/*.h src/platform/pocket/include/*.h src/game/assets.def)
	@mkdir -p $(BUILD)/pocket
	$(RISCV)gcc $(POCKET_FLAGS) -nostdlib -Wl,--gc-sections -Wl,-Map,$(BUILD)/pocket/taikopocket.map \
		-T src/platform/pocket/link.ld -o $(BUILD)/pocket/taikopocket.elf src/platform/pocket/crt0.S $(POCKET_SRC)
	$(RISCV)objcopy -O binary $(BUILD)/pocket/taikopocket.elf $@
	$(RISCV)size $(BUILD)/pocket/taikopocket.elf

# Golden test against YataiDON's own parser (needs third_party/YataiDON, a C++23 compiler).
YATAI := third_party/YataiDON/src
CXX ?= c++

$(BUILD)/tjadump: tools/tjadump.c $(CORE_OBJ)
	$(CC) $(CFLAGS) -Wno-missing-field-initializers -o $@ $^ -lm

$(BUILD)/yatai/yatai_dump: tests/yatai/yatai_dump.cpp tests/yatai/stubs/stubs.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=c++23 -O1 -w -Itests/yatai/stubs -I$(YATAI) -o $@ $^ \
		$(YATAI)/libs/parsers/tja.cpp $(YATAI)/libs/judge.cpp $(YATAI)/libs/encoding.cpp

$(BUILD)/judgesim: tools/judgesim.c $(CORE_OBJ)
	$(CC) $(CFLAGS) -Wno-missing-field-initializers -o $@ $^ -lm

$(BUILD)/yatai/yatai_judge: tests/yatai/yatai_judge.cpp tests/yatai/stubs/stubs.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=c++23 -O1 -w -Itests/yatai/stubs -I$(YATAI) -o $@ $^ \
		$(YATAI)/libs/parsers/tja.cpp $(YATAI)/libs/judge.cpp $(YATAI)/libs/encoding.cpp

# parser: identical note dumps; judge: identical results for simulated plays
golden: $(BUILD)/tjadump $(BUILD)/yatai/yatai_dump $(BUILD)/judgesim $(BUILD)/yatai/yatai_judge
	$(PYTHON) tests/golden.py
	$(PYTHON) tests/replay.py

run: $(BUILD)/taikopocket $(BUILD)/tests/songs.tpl
	$(BUILD)/taikopocket $(BUILD)/tests/songs.tpl

sim:
	$(MAKE) -C gateware/taiko sim

# everything this Makefile builds; the Python environment, downloads and packed libraries stay
clean:
	rm -rf $(BUILD)/obj $(BUILD)/tests $(BUILD)/taikopocket $(BUILD)/tplist $(BUILD)/tplcheck $(BUILD)/tjadump \
		$(BUILD)/judgesim $(BUILD)/yatai $(BUILD)/golden $(BUILD)/replay $(BUILD)/gateware $(BUILD)/web $(BUILD)/pocket

-include $(APP_OBJ:.o=.d)
