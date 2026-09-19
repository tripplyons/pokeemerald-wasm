#include "native_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdint.h>

#define FLASH_BASE 0x0e000000u
#define MEMORY_SIZE 0x10000000u
#define PAGE_BITS 24
#define PAGE_SIZE (1u << PAGE_BITS)

extern void AgbMain(void);
extern void WasmRunFrame(void);
extern void WasmRenderFrame(void);
extern uint8_t *WasmDisplayBuffer(void);
extern uint32_t WasmCheckSpriteSort(void);

extern void NativeInit_maps(void);
extern void NativeInit_map_events(void);
extern void NativeInit_event_scripts(void);
extern void NativeInit_battle_scripts_1(void);
extern void NativeInit_battle_scripts_2(void);
extern void NativeInit_battle_ai_scripts(void);
extern void NativeInit_battle_anim_scripts(void);

extern void NativeInit_contest_ai_scripts(void);
extern void NativeInit_mystery_event_script_cmd_table(void);
extern void NativeInit_multiboot_pokemon_colosseum(void);
extern void NativeInit_multiboot_ereader(void);
extern void NativeInit_multiboot_berry_glitch_fix(void);

struct NativeEngine { uint8_t *memory; };
unsigned char gNativeMemory[MEMORY_SIZE];
#ifdef __APPLE__
extern uint8_t gameBssStart[] __asm__("section$start$__DATA$native_game");
extern uint8_t gameBssEnd[] __asm__("section$end$__DATA$native_game");
extern uint8_t gameDataStart[] __asm__("section$start$__DATA$native_game_data");
extern uint8_t gameDataEnd[] __asm__("section$end$__DATA$native_game_data");
#else
extern uint8_t gameBssStart[] __asm__("__start_native_game");
extern uint8_t gameBssEnd[] __asm__("__stop_native_game");
extern uint8_t gameDataStart[] __asm__("__start_native_game_data");
extern uint8_t gameDataEnd[] __asm__("__stop_native_game_data");
#endif
static uint8_t *initialGameData;
static NativeEngine *activeEngine;
static uintptr_t pointerPages[256];
static unsigned int pageCount = 16;

uint32_t NativePointerToWord(const void *pointer)
{
    uintptr_t address = (uintptr_t)pointer;
    if (!address) return 0;
    uintptr_t memory = (uintptr_t)activeEngine->memory;
    if (address >= memory && address - memory < MEMORY_SIZE)
        return address - memory;
    uintptr_t page = address & ~(uintptr_t)(PAGE_SIZE - 1);
    for (unsigned int i = 16; i < pageCount; i++)
        if (pointerPages[i] == page)
            return (i << PAGE_BITS) | (address & (PAGE_SIZE - 1));
    if (pageCount == 256) {
        fprintf(stderr, "native pointer address space exhausted\n");
        abort();
    }
    // Reserve adjacent pages together: game code also does arithmetic on
    // encoded addresses, including copies that cross a 16 MiB page boundary.
    unsigned int count = 256 - pageCount;
    if (count > 64) count = 64;
    unsigned int center = count / 2;
    uintptr_t firstPage = page - (uintptr_t)center * PAGE_SIZE;
    unsigned int slot = pageCount + center;
    for (unsigned int i = 0; i < count; i++)
        pointerPages[pageCount + i] = firstPage + (uintptr_t)i * PAGE_SIZE;
    pageCount += count;
    return (slot << PAGE_BITS) | (address & (PAGE_SIZE - 1));
}

void *NativeDecodePointer(unsigned long word)
{
    if (!word) return NULL;
    if (word < MEMORY_SIZE) return activeEngine->memory + word;
    unsigned int page = (uint32_t)word >> PAGE_BITS;
    if (page >= pageCount) {
        fprintf(stderr, "invalid native pointer word: %lx\n", word);
        abort();
    }
    return (void *)(pointerPages[page] + (word & (PAGE_SIZE - 1)));
}

static uint32_t hash_bytes(const uint8_t *bytes, size_t size)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static uint64_t hash_bytes64(const uint8_t *bytes, size_t size)
{
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static void ensure_save_dir(void)
{
    mkdir("build", 0777);
    mkdir("build/native", 0777);
}


NativeEngine *native_engine_create(void)
{
    if (activeEngine) return NULL;
    NativeEngine *engine = calloc(1, sizeof(*engine));
    if (!engine) return NULL;
    engine->memory = gNativeMemory;
    memset(gNativeMemory, 0, sizeof(gNativeMemory));
    size_t dataSize = gameDataEnd - gameDataStart;
    if (!initialGameData) {
        initialGameData = malloc(dataSize);
        if (!initialGameData) { free(engine); return NULL; }
        memcpy(initialGameData, gameDataStart, dataSize);
    }
    memcpy(gameDataStart, initialGameData, dataSize);
    memset(gameBssStart, 0, gameBssEnd - gameBssStart);
    activeEngine = engine;
    NativeInit_contest_ai_scripts();
    NativeInit_mystery_event_script_cmd_table();
    NativeInit_multiboot_pokemon_colosseum();
    NativeInit_multiboot_ereader();
    NativeInit_multiboot_berry_glitch_fix();
    NativeInit_maps();
    NativeInit_map_events();
    NativeInit_event_scripts();
    NativeInit_battle_scripts_1();
    NativeInit_battle_scripts_2();
    NativeInit_battle_ai_scripts();
    NativeInit_battle_anim_scripts();
    memset(engine->memory + FLASH_BASE, 0xff, NATIVE_FLASH_SIZE);
    return engine;
}

void native_engine_boot(NativeEngine *engine)
{
    native_engine_set_keys(engine, 0);
    AgbMain();
}

void native_engine_destroy(NativeEngine *engine)
{
    if (!engine) return;
    free(engine);
    activeEngine = NULL;
}

void native_engine_set_keys(NativeEngine *engine, uint32_t held)
{
    uint16_t value = NATIVE_BUTTON_MASK ^ (held & NATIVE_BUTTON_MASK);
    memcpy(engine->memory + 0x04000130, &value, sizeof(value));
}

void native_engine_run_frame(NativeEngine *engine) { (void)engine; WasmRunFrame(); }
void native_engine_render(NativeEngine *engine) { (void)engine; WasmRenderFrame(); }
const uint8_t *native_engine_display_buffer(const NativeEngine *engine) { (void)engine; return WasmDisplayBuffer(); }

uint64_t native_engine_hash_display(const NativeEngine *engine)
{
    return hash_bytes64(native_engine_display_buffer(engine), NATIVE_DISPLAY_BYTES);
}

uint32_t native_engine_load_flash(NativeEngine *engine, const char *path)
{
    uint8_t *flash = engine->memory + FLASH_BASE;
    memset(flash, 0xff, NATIVE_FLASH_SIZE);

    if (path) {
        FILE *file = fopen(path, "rb");
        if (file) {
            fseek(file, 0, SEEK_END);
            long size = ftell(file);
            rewind(file);
            if (size == NATIVE_FLASH_SIZE)
                (void)fread(flash, 1, NATIVE_FLASH_SIZE, file);
            fclose(file);
        }
    }

    return hash_bytes(flash, NATIVE_FLASH_SIZE);
}

uint32_t native_engine_save_flash_if_changed(NativeEngine *engine, const char *path, uint32_t lastHash, bool force)
{
    uint8_t *flash = engine->memory + FLASH_BASE;
    uint32_t hash = hash_bytes(flash, NATIVE_FLASH_SIZE);
    if (!force && hash == lastHash)
        return lastHash;

    ensure_save_dir();
    FILE *file = fopen(path, "wb");
    if (file) {
        (void)fwrite(flash, 1, NATIVE_FLASH_SIZE, file);
        fclose(file);
        return hash;
    }

    return lastHash;
}

uint32_t native_engine_check_sprite_sort(NativeEngine *engine)
{
    return WasmCheckSpriteSort();
}

extern uint32_t WasmBattleShortcutCount(void);
extern uint32_t WasmBattleShortcutType(uint32_t index);
extern uint32_t WasmBattleShortcutLabel(uint32_t index);
extern void WasmBattleShortcutSelect(uint32_t index);

uint32_t native_engine_battle_shortcut_count(NativeEngine *engine) { (void)engine; return WasmBattleShortcutCount(); }
uint32_t native_engine_battle_shortcut_type(NativeEngine *engine, uint32_t index) { (void)engine; return WasmBattleShortcutType(index); }
const char *native_engine_battle_shortcut_label(NativeEngine *engine, uint32_t index) { (void)engine; return NativeDecodePointer(WasmBattleShortcutLabel(index)); }
void native_engine_battle_shortcut_select(NativeEngine *engine, uint32_t index) { (void)engine; WasmBattleShortcutSelect(index); }
