// Headless core of the pokeemerald wasm2c native port.
//
// This module owns the wasm2c instance, the GBA BIOS/syscall shims
// (w2c_env_*), pad input, flash save storage, and framebuffer access.
// It intentionally has no raylib or other GUI dependency so the engine
// can be driven by the raylib frontend, the headless benchmark, or any
// other host.
#ifndef POKEEMERALD_NATIVE_ENGINE_H
#define POKEEMERALD_NATIVE_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NATIVE_DISPLAY_WIDTH 240
#define NATIVE_DISPLAY_HEIGHT 160
#define NATIVE_DISPLAY_BYTES (NATIVE_DISPLAY_WIDTH * NATIVE_DISPLAY_HEIGHT * 4)
#define NATIVE_FLASH_SIZE (128u * 1024u)

#define NATIVE_BUTTON_A      (1u << 0)
#define NATIVE_BUTTON_B      (1u << 1)
#define NATIVE_BUTTON_SELECT (1u << 2)
#define NATIVE_BUTTON_START  (1u << 3)
#define NATIVE_BUTTON_RIGHT  (1u << 4)
#define NATIVE_BUTTON_LEFT   (1u << 5)
#define NATIVE_BUTTON_UP     (1u << 6)
#define NATIVE_BUTTON_DOWN   (1u << 7)
#define NATIVE_BUTTON_R      (1u << 8)
#define NATIVE_BUTTON_L      (1u << 9)
#define NATIVE_BUTTON_MASK   0x03ffu

typedef struct NativeEngine NativeEngine;

// Creates an engine instance with an erased (0xff) flash save.
// Load a save with native_engine_load_flash(), then call
// native_engine_boot() exactly once before running frames.
NativeEngine *native_engine_create(void);

// Runs the module's AgbMain() setup entry with no keys held.
void native_engine_boot(NativeEngine *engine);

void native_engine_destroy(NativeEngine *engine);

// Updates the emulated KEYINPUT register. Bits use NATIVE_BUTTON_*.
void native_engine_set_keys(NativeEngine *engine, uint32_t held);

// Advances the game by one emulated frame (WasmRunFrame).
void native_engine_run_frame(NativeEngine *engine);

// Composites the current GBA frame into the RGBA display buffer.
void native_engine_render(NativeEngine *engine);

// Returns the RGBA display buffer (NATIVE_DISPLAY_BYTES bytes).
const uint8_t *native_engine_display_buffer(const NativeEngine *engine);

// FNV-1a 64-bit hash of the current RGBA display buffer.
uint64_t native_engine_hash_display(const NativeEngine *engine);

// Loads a 128 KiB flash save from path into emulated flash. A NULL or
// missing path leaves flash erased (0xff). Returns the flash hash.
uint32_t native_engine_load_flash(NativeEngine *engine, const char *path);

// Writes flash to path when its hash differs from lastHash (or force is
// set). Returns the new last-hash value.
uint32_t native_engine_save_flash_if_changed(NativeEngine *engine, const char *path, uint32_t lastHash, bool force);

#endif // POKEEMERALD_NATIVE_ENGINE_H
