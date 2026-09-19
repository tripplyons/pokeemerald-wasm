comma := ,
NATIVE_AST_CC ?= clang
NATIVE_AST_TARGET := $(shell $(NATIVE_CC) $(NATIVE_CFLAGS) -dumpmachine)
NATIVE_MACROS := $(shell $(NATIVE_CC) $(NATIVE_CFLAGS) -dM -E -x c /dev/null)
NATIVE_FORMAT := $(if $(findstring __APPLE__,$(NATIVE_MACROS)),macho,elf)
NATIVE_POINTER_SIZE ?= $(shell $(NATIVE_CC) $(NATIVE_CFLAGS) -dM -E -x c /dev/null | sed -n 's/^\#define __SIZEOF_POINTER__ //p')
NATIVE_DATA_NAMES := $(basename $(notdir $(WASM_DATA_ASM_SRCS))) contest_ai_scripts mystery_event_script_cmd_table multiboot_pokemon_colosseum multiboot_ereader multiboot_berry_glitch_fix
NATIVE_DATA_OBJS := $(addprefix $(NATIVE_BUILD_DIR)/data/,$(addsuffix .o,$(NATIVE_DATA_NAMES)))
NATIVE_BIOS_O := $(NATIVE_BUILD_DIR)/bios.o
NATIVE_OBJ_DIR := $(NATIVE_BUILD_DIR)/obj
NATIVE_C_OBJS := $(patsubst $(C_SUBDIR)/%.c,$(NATIVE_OBJ_DIR)/%.o,$(C_SRCS))
NATIVE_C_OBJS += $(NATIVE_OBJ_DIR)/native_game_save.o $(NATIVE_OBJ_DIR)/native_platform.o
$(NATIVE_OBJ_DIR)/native_%.o: tools/native/%.c
	@mkdir -p $(dir $@)
	$(NATIVE_CC) $(NATIVE_CFLAGS) $(NATIVE_GAME_FLAGS) $(NATIVE_INCLUDES) -MMD -MP -MF $@.d -MT $@ -E $< | $(PREPROC) -i -g $(ASSETS_DIR_NAME) $< charmap.txt > $@.i
	python3 tools/native/prepare_source.py $@.i $@.c $(NATIVE_FORMAT) $(NATIVE_AST_CC) --target=$(NATIVE_AST_TARGET)
	$(NATIVE_CC) $(NATIVE_CFLAGS) $(NATIVE_GAME_FLAGS) -x c -c $@.c -o $@

NATIVE_GAME_FLAGS := -DMODERN=1 -DWASM=1 -DNATIVE=1 -DNDEBUG -fno-strict-aliasing -fwrapv -Wno-everything
NATIVE_INCLUDES := -I $(WASM_BUILD_DIR) -I include/wasm -I include -iquote include

$(NATIVE_C_OBJS): | generated wasm-assets
$(NATIVE_OBJ_DIR)/m4a.o: $(WASM_SOUND_HEADER)

$(NATIVE_OBJ_DIR)/%.o: $(C_SUBDIR)/%.c tools/native/prepare_source.py Makefile tools/native/build.mk
	@mkdir -p $(dir $@)
	$(NATIVE_CC) $(NATIVE_CFLAGS) $(NATIVE_GAME_FLAGS) $(NATIVE_INCLUDES) -MMD -MP -MF $@.d -MT $@ -E $< | $(PREPROC) -i -g $(ASSETS_DIR_NAME) $< charmap.txt > $@.i
	python3 tools/native/prepare_source.py $@.i $@.c $(NATIVE_FORMAT) $(NATIVE_AST_CC) --target=$(NATIVE_AST_TARGET)
	$(NATIVE_CC) $(NATIVE_CFLAGS) $(NATIVE_GAME_FLAGS) -x c -c $@.c -o $@

$(NATIVE_ENGINE_O): tools/native/native_engine.c $(NATIVE_ENGINE_H)
	@mkdir -p $(dir $@)
	$(NATIVE_CC) $(NATIVE_CFLAGS) -c $< -o $@

$(NATIVE_RAYLIB_MAIN_O): tools/native/raylib_main.c $(NATIVE_ENGINE_H)
	@mkdir -p $(dir $@)
	$(NATIVE_CC) $(NATIVE_CFLAGS) $(RAYLIB_CFLAGS) -c $< -o $@

$(NATIVE_BENCH_O): tools/native/bench_main.c $(NATIVE_ENGINE_H)
	@mkdir -p $(dir $@)
	$(NATIVE_CC) $(NATIVE_CFLAGS) -c $< -o $@

$(NATIVE_RAYLIB): $(NATIVE_C_OBJS) $(NATIVE_ENGINE_O) $(NATIVE_BIOS_O) $(NATIVE_DATA_OBJS) $(NATIVE_RAYLIB_MAIN_O)
	$(NATIVE_CC) $(NATIVE_CFLAGS) $(NATIVE_LDFLAGS) $(if $(filter macho,$(NATIVE_FORMAT)),-Wl$(comma)-no_fixup_chains) -o $@ $^ $(RAYLIB_LIBS) -lm

$(NATIVE_BENCH): $(NATIVE_C_OBJS) $(NATIVE_ENGINE_O) $(NATIVE_BIOS_O) $(NATIVE_DATA_OBJS) $(NATIVE_BENCH_O)
	$(NATIVE_CC) $(NATIVE_CFLAGS) $(NATIVE_LDFLAGS) $(if $(filter macho,$(NATIVE_FORMAT)),-Wl$(comma)-no_fixup_chains) -o $@ $^ -lm

$(NATIVE_BIOS_O): tools/native/bios.c
	@mkdir -p $(dir $@)
	$(NATIVE_CC) $(NATIVE_CFLAGS) -c $< -o $@

$(NATIVE_BUILD_DIR)/data/%.o: data/%.s tools/native/generate_data.py tools/wasm_asm_data.py | generated
	@mkdir -p $(dir $@)
	$(SCANINC) -M $(@:.o=.d) -I include -I "" $<
	python3 tools/native/generate_data.py $< $@.c --pointer-size $(NATIVE_POINTER_SIZE) --format $(NATIVE_FORMAT)
# Keep assembly aliases and untyped symbol-address relocations outside LTO.
	$(NATIVE_CC) $(NATIVE_CFLAGS) -fno-lto -c $@.c -o $@

$(NATIVE_C_OBJS): tools/native/prepare_source.py
$(NATIVE_C_OBJS) $(NATIVE_DATA_OBJS) $(NATIVE_ENGINE_O) $(NATIVE_BIOS_O) $(NATIVE_RAYLIB_MAIN_O) $(NATIVE_BENCH_O) $(KINDLE_FRONTEND_O): tools/native/build.mk tools/native/performance.mk Makefile
-include $(NATIVE_C_OBJS:.o=.o.d) $(NATIVE_DATA_OBJS:.o=.d)

ifeq ($(KINDLE_BUILD),1)
$(KINDLE_FRONTEND_O): tools/native/kindle_main.c $(NATIVE_ENGINE_H)
	@mkdir -p $(dir $@)
	$(NATIVE_CC) $(NATIVE_CFLAGS) -c $< -o $@

$(NATIVE_KINDLE): $(NATIVE_C_OBJS) $(NATIVE_ENGINE_O) $(NATIVE_BIOS_O) $(NATIVE_DATA_OBJS) $(KINDLE_FRONTEND_O)
	$(NATIVE_CC) $(NATIVE_CFLAGS) $(NATIVE_LDFLAGS) $(if $(filter macho,$(NATIVE_FORMAT)),-Wl$(comma)-no_fixup_chains) -o $@ $^ -lm
endif

.PHONY: native-test
native-test: $(NATIVE_BUILD_DIR)/pokeemerald-test
	python3 tools/native/test_prepare_source.py
	$(NATIVE_BUILD_DIR)/pokeemerald-test

$(NATIVE_OBJ_DIR)/native_test_game.o: tools/native/prepare_source.py tools/native/build.mk Makefile | generated wasm-assets
-include $(NATIVE_OBJ_DIR)/native_test_game.o.d

$(NATIVE_BUILD_DIR)/test_main.o: tools/native/test_main.c $(NATIVE_ENGINE_H)
	@mkdir -p $(dir $@)
	$(NATIVE_CC) $(NATIVE_CFLAGS) -c $< -o $@

$(NATIVE_BUILD_DIR)/pokeemerald-test: $(NATIVE_C_OBJS) $(NATIVE_ENGINE_O) $(NATIVE_BIOS_O) $(NATIVE_DATA_OBJS) $(NATIVE_OBJ_DIR)/native_test_game.o $(NATIVE_BUILD_DIR)/test_main.o
	$(NATIVE_CC) $(NATIVE_CFLAGS) $(NATIVE_LDFLAGS) $(if $(filter macho,$(NATIVE_FORMAT)),-Wl$(comma)-no_fixup_chains) -o $@ $^ -lm
