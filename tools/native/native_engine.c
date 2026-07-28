// Headless engine core; see native_engine.h for the contract.
// The w2c_env_* functions below are the host-side implementations of
// the GBA BIOS calls and sound stubs imported by the wasm2c module.
#include "native_engine.h"
#include "pokeemerald_wasm2c.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define REG_KEYINPUT 0x04000130u
#define FLASH_BASE 0x0e000000u

typedef w2c_0x24pokeemerald0x2Ewasm Pokeemerald;

struct w2c_env {
    Pokeemerald *instance;
};

struct NativeEngine {
    Pokeemerald instance;
    struct w2c_env env;
};

static wasm_rt_memory_t *memory_for(struct w2c_env *env)
{
    return w2c_0x24pokeemerald0x2Ewasm_memory(env->instance);
}

static uint8_t *memory_data(struct w2c_env *env)
{
    return memory_for(env)->data;
}

static bool valid_range(struct w2c_env *env, uint32_t addr, uint32_t size)
{
    wasm_rt_memory_t *memory = memory_for(env);
    return addr <= memory->size && size <= memory->size - addr;
}

static uint16_t read_u16(struct w2c_env *env, uint32_t addr)
{
    uint8_t *mem = memory_data(env);
    return (uint16_t)(mem[addr] | (mem[addr + 1] << 8));
}

static int16_t read_s16(struct w2c_env *env, uint32_t addr)
{
    return (int16_t)read_u16(env, addr);
}

static uint32_t read_u32(struct w2c_env *env, uint32_t addr)
{
    uint8_t *mem = memory_data(env);
    return (uint32_t)mem[addr]
        | ((uint32_t)mem[addr + 1] << 8)
        | ((uint32_t)mem[addr + 2] << 16)
        | ((uint32_t)mem[addr + 3] << 24);
}

static int32_t read_s32(struct w2c_env *env, uint32_t addr)
{
    return (int32_t)read_u32(env, addr);
}

static void write_u16(struct w2c_env *env, uint32_t addr, uint16_t value)
{
    uint8_t *mem = memory_data(env);
    mem[addr] = value & 0xff;
    mem[addr + 1] = value >> 8;
}

static void write_u32(struct w2c_env *env, uint32_t addr, uint32_t value)
{
    uint8_t *mem = memory_data(env);
    mem[addr] = value & 0xff;
    mem[addr + 1] = (value >> 8) & 0xff;
    mem[addr + 2] = (value >> 16) & 0xff;
    mem[addr + 3] = value >> 24;
}

static void write_s16(struct w2c_env *env, uint32_t addr, int32_t value)
{
    write_u16(env, addr, (uint16_t)value);
}

static void copy_units(struct w2c_env *env, uint32_t src, uint32_t dst, uint32_t count, uint32_t size, bool fill)
{
    uint8_t *mem = memory_data(env);

    if (!valid_range(env, src, size) || !valid_range(env, dst, count * size))
        return;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t from = fill ? src : src + i * size;
        memmove(mem + dst + i * size, mem + from, size);
    }
}

static void lz77(struct w2c_env *env, uint32_t src, uint32_t dst)
{
    uint8_t *mem = memory_data(env);
    uint32_t size = mem[src + 1] | (mem[src + 2] << 8) | (mem[src + 3] << 16);
    uint32_t s = src + 4;
    uint32_t d = dst;
    uint32_t end = dst + size;

    if (!valid_range(env, dst, size))
        return;

    while (d < end) {
        uint8_t flags = mem[s++];
        for (int bit = 7; bit >= 0 && d < end; bit--) {
            if (flags & (1 << bit)) {
                uint32_t pair = ((uint32_t)mem[s] << 8) | mem[s + 1];
                s += 2;
                uint32_t length = (pair >> 12) + 3;
                uint32_t disp = (pair & 0xfff) + 1;
                while (length-- && d < end) {
                    mem[d] = mem[d - disp];
                    d++;
                }
            } else {
                mem[d++] = mem[s++];
            }
        }
    }
}

static void rl(struct w2c_env *env, uint32_t src, uint32_t dst)
{
    uint8_t *mem = memory_data(env);
    uint32_t size = mem[src + 1] | (mem[src + 2] << 8) | (mem[src + 3] << 16);
    uint32_t s = src + 4;
    uint32_t d = dst;
    uint32_t end = dst + size;

    if (!valid_range(env, dst, size))
        return;

    while (d < end) {
        uint8_t flag = mem[s++];
        if (flag & 0x80) {
            uint32_t count = (flag & 0x7f) + 3;
            uint8_t value = mem[s++];
            while (count-- && d < end)
                mem[d++] = value;
        } else {
            uint32_t count = (flag & 0x7f) + 1;
            while (count-- && d < end)
                mem[d++] = mem[s++];
        }
    }
}

static void bg_affine_set(struct w2c_env *env, uint32_t src, uint32_t dst, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        uint32_t s = src + i * 20;
        uint32_t d = dst + i * 16;
        int32_t texX = read_s32(env, s);
        int32_t texY = read_s32(env, s + 4);
        int16_t scrX = read_s16(env, s + 8);
        int16_t scrY = read_s16(env, s + 10);
        int16_t xScale = read_s16(env, s + 12);
        int16_t yScale = read_s16(env, s + 14);
        uint16_t rotation = read_u16(env, s + 16);
        double angle = rotation * M_PI * 2.0 / 0x10000;
        double sn = sin(angle) * 256.0;
        double cs = cos(angle) * 256.0;
        int32_t a = (int32_t)(cs * xScale / 256.0);
        int32_t b = (int32_t)(-sn * xScale / 256.0);
        int32_t c = (int32_t)(sn * yScale / 256.0);
        int32_t e = (int32_t)(cs * yScale / 256.0);
        write_s16(env, d, a);
        write_s16(env, d + 2, b);
        write_s16(env, d + 4, c);
        write_s16(env, d + 6, e);
        write_u32(env, d + 8, (uint32_t)(texX - scrX * a - scrY * b));
        write_u32(env, d + 12, (uint32_t)(texY - scrX * c - scrY * e));
    }
}

static void obj_affine_set(struct w2c_env *env, uint32_t src, uint32_t dst, uint32_t count, uint32_t offset)
{
    uint8_t *mem = memory_data(env);
    const uint8_t *source = mem + src;
    uint8_t *output = mem + dst;

    for (uint32_t i = 0; i < count; i++) {
        int16_t xScale;
        int16_t yScale;
        uint16_t rotation;
        int16_t value;
        memcpy(&xScale, source, sizeof(xScale));
        memcpy(&yScale, source + 2, sizeof(yScale));
        memcpy(&rotation, source + 4, sizeof(rotation));
        double angle = rotation * M_PI * 2.0 / 0x10000;
        double sn = sin(angle) * 256.0;
        double cs = cos(angle) * 256.0;
        value = (int32_t)(cs * xScale / 256.0);
        memcpy(output, &value, sizeof(value));
        value = (int32_t)(-sn * xScale / 256.0);
        memcpy(output + offset, &value, sizeof(value));
        value = (int32_t)(sn * yScale / 256.0);
        memcpy(output + offset * 2, &value, sizeof(value));
        value = (int32_t)(cs * yScale / 256.0);
        memcpy(output + offset * 3, &value, sizeof(value));
        source += 6;
        output += offset * 4;
    }
}

static void copy_oam_matrices(struct w2c_env *env, uint32_t src, uint32_t dest)
{
    uint8_t *mem = memory_data(env);
    uint8_t *restrict source;
    uint8_t *restrict output;

    if (!valid_range(env, src, 32 * 8) || !valid_range(env, dest, 128 * 8))
        return;

    source = mem + src;
    output = mem + dest + 6;
    for (uint32_t matrix = 0; matrix < 32; matrix++) {
        uint16_t value;
        memcpy(&value, source, sizeof(value));
        memcpy(output, &value, sizeof(value));
        memcpy(&value, source + 2, sizeof(value));
        memcpy(output + 8, &value, sizeof(value));
        memcpy(&value, source + 4, sizeof(value));
        memcpy(output + 16, &value, sizeof(value));
        memcpy(&value, source + 6, sizeof(value));
        memcpy(output + 24, &value, sizeof(value));
        source += 8;
        output += 32;
    }
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

static uint8_t *flash_bytes(Pokeemerald *instance)
{
    return w2c_0x24pokeemerald0x2Ewasm_memory(instance)->data + FLASH_BASE;
}

u32 w2c_env_ArcTan2(struct w2c_env *env, u32 x, u32 y)
{
    (void)env;
    double angle = atan2((double)(int32_t)y, (double)(int32_t)x);
    if (angle < 0.0)
        angle += M_PI * 2.0;
    return (u32)(angle * 65536.0 / (M_PI * 2.0));
}

void w2c_env_BgAffineSet(struct w2c_env *env, u32 src, u32 dest, u32 count) { bg_affine_set(env, src, dest, count); }
void w2c_env_CpuFastSet(struct w2c_env *env, u32 src, u32 dest, u32 mode) { copy_units(env, src, dest, mode & 0x1fffff, 4, (mode >> 24) & 1); }
void w2c_env_CpuSet(struct w2c_env *env, u32 src, u32 dest, u32 mode) { copy_units(env, src, dest, mode & 0x1fffff, ((mode >> 26) & 1) ? 4 : 2, (mode >> 24) & 1); }
u32 w2c_env_Div(struct w2c_env *env, u32 num, u32 den) { (void)env; return den ? (u32)((int32_t)num / (int32_t)den) : 0; }
void w2c_env_LZ77UnCompVram(struct w2c_env *env, u32 src, u32 dest) { lz77(env, src, dest); }
void w2c_env_LZ77UnCompWram(struct w2c_env *env, u32 src, u32 dest) { lz77(env, src, dest); }
void w2c_env_ObjAffineSet(struct w2c_env *env, u32 src, u32 dest, u32 count, u32 offset) { obj_affine_set(env, src, dest, count, offset); }
void w2c_env_WasmCopyOamMatrices(struct w2c_env *env, u32 src, u32 dest) { copy_oam_matrices(env, src, dest); }
void w2c_env_RLUnCompVram(struct w2c_env *env, u32 src, u32 dest) { rl(env, src, dest); }
void w2c_env_RLUnCompWram(struct w2c_env *env, u32 src, u32 dest) { rl(env, src, dest); }
u32 w2c_env_Sqrt(struct w2c_env *env, u32 value) { (void)env; return (u32)sqrt((double)value); }
u32 w2c_env_strcmp(struct w2c_env *env, u32 a, u32 b) { return (u32)strcmp((const char *)memory_data(env) + a, (const char *)memory_data(env) + b); }

void w2c_env_FadeOutBody(struct w2c_env *env, u32 a) { (void)env; (void)a; }
void w2c_env_GameCubeMultiBoot_ExecuteProgram(struct w2c_env *env, u32 a) { (void)env; (void)a; }
void w2c_env_GameCubeMultiBoot_HandleSerialInterrupt(struct w2c_env *env, u32 a) { (void)env; (void)a; }
void w2c_env_GameCubeMultiBoot_Init(struct w2c_env *env, u32 a) { (void)env; (void)a; }
void w2c_env_GameCubeMultiBoot_Main(struct w2c_env *env, u32 a) { (void)env; (void)a; }
void w2c_env_GameCubeMultiBoot_Quit(struct w2c_env *env) { (void)env; }
u32 w2c_env_IsPokemonCryPlaying(struct w2c_env *env, u32 a) { (void)env; (void)a; return 0; }
u32 w2c_env_MultiBoot(struct w2c_env *env, u32 a) { (void)env; (void)a; return 0; }
void w2c_env_RealClearChain(struct w2c_env *env, u32 a) { (void)env; (void)a; }
void w2c_env_RegisterRamReset(struct w2c_env *env, u32 a) { (void)env; (void)a; }
void w2c_env_SampleFreqSet(struct w2c_env *env, u32 a) { (void)env; (void)a; }
void w2c_env_SetPokemonCryChorus(struct w2c_env *env, u32 a) { (void)env; (void)a; }
void w2c_env_SetPokemonCryLength(struct w2c_env *env, u32 a) { (void)env; (void)a; }
void w2c_env_SetPokemonCryPanpot(struct w2c_env *env, u32 a) { (void)env; (void)a; }
void w2c_env_SetPokemonCryPitch(struct w2c_env *env, u32 a) { (void)env; (void)a; }
void w2c_env_SetPokemonCryProgress(struct w2c_env *env, u32 a) { (void)env; (void)a; }
void w2c_env_SetPokemonCryRelease(struct w2c_env *env, u32 a) { (void)env; (void)a; }
void w2c_env_SetPokemonCryStereo(struct w2c_env *env, u32 a) { (void)env; (void)a; }
u32 w2c_env_SetPokemonCryTone(struct w2c_env *env, u32 a) { (void)env; (void)a; return 0; }
void w2c_env_SetPokemonCryVolume(struct w2c_env *env, u32 a) { (void)env; (void)a; }
void w2c_env_SoftReset(struct w2c_env *env, u32 a) { (void)env; (void)a; }
void w2c_env_TrackStop(struct w2c_env *env, u32 a, u32 b) { (void)env; (void)a; (void)b; }
void w2c_env_TrkVolPitSet(struct w2c_env *env, u32 a, u32 b) { (void)env; (void)a; (void)b; }
void w2c_env_VBlankIntrWait(struct w2c_env *env) { (void)env; }

#define NOOP_PLY(name) void w2c_env_##name(struct w2c_env *env, u32 a, u32 b) { (void)env; (void)a; (void)b; }
NOOP_PLY(ply_bend)
NOOP_PLY(ply_bendr)
NOOP_PLY(ply_endtie)
NOOP_PLY(ply_fine)
NOOP_PLY(ply_goto)
NOOP_PLY(ply_keysh)
NOOP_PLY(ply_lfodl)
NOOP_PLY(ply_lfos)
NOOP_PLY(ply_mod)
NOOP_PLY(ply_modt)
NOOP_PLY(ply_pan)
NOOP_PLY(ply_patt)
NOOP_PLY(ply_pend)
NOOP_PLY(ply_port)
NOOP_PLY(ply_prio)
NOOP_PLY(ply_rept)
NOOP_PLY(ply_tempo)
NOOP_PLY(ply_tune)
NOOP_PLY(ply_voice)
NOOP_PLY(ply_vol)
NOOP_PLY(ply_xatta)
NOOP_PLY(ply_xcmd_0D)
NOOP_PLY(ply_xdeca)
NOOP_PLY(ply_xiecl)
NOOP_PLY(ply_xiecv)
NOOP_PLY(ply_xleng)
NOOP_PLY(ply_xrele)
NOOP_PLY(ply_xsust)
NOOP_PLY(ply_xswee)
NOOP_PLY(ply_xtype)
NOOP_PLY(ply_xwait)
NOOP_PLY(ply_xwave)
NOOP_PLY(ply_xxx)
#undef NOOP_PLY

static void write_keys(Pokeemerald *instance, uint32_t held)
{
    uint8_t *mem = w2c_0x24pokeemerald0x2Ewasm_memory(instance)->data;
    uint16_t value = NATIVE_BUTTON_MASK ^ (held & NATIVE_BUTTON_MASK);
    mem[REG_KEYINPUT] = value & 0xff;
    mem[REG_KEYINPUT + 1] = value >> 8;
}

NativeEngine *native_engine_create(void)
{
    static bool runtimeInitialized = false;
    if (!runtimeInitialized) {
        wasm_rt_init();
        runtimeInitialized = true;
    }

    NativeEngine *engine = calloc(1, sizeof(*engine));
    if (!engine)
        return NULL;

    engine->env.instance = &engine->instance;
    wasm2c_0x24pokeemerald0x2Ewasm_instantiate(&engine->instance, &engine->env);

    uint8_t *flash = flash_bytes(&engine->instance);
    memset(flash, 0xff, NATIVE_FLASH_SIZE);

    return engine;
}

void native_engine_boot(NativeEngine *engine)
{
    write_keys(&engine->instance, 0);
    w2c_0x24pokeemerald0x2Ewasm_AgbMain(&engine->instance);
}

void native_engine_destroy(NativeEngine *engine)
{
    if (!engine)
        return;
    wasm2c_0x24pokeemerald0x2Ewasm_free(&engine->instance);
    free(engine);
}

void native_engine_set_keys(NativeEngine *engine, uint32_t held)
{
    write_keys(&engine->instance, held);
}

void native_engine_run_frame(NativeEngine *engine)
{
    w2c_0x24pokeemerald0x2Ewasm_WasmRunFrame(&engine->instance);
}

void native_engine_render(NativeEngine *engine)
{
    w2c_0x24pokeemerald0x2Ewasm_WasmRenderFrame(&engine->instance);
}

const uint8_t *native_engine_display_buffer(const NativeEngine *engine)
{
    Pokeemerald *instance = (Pokeemerald *)&engine->instance;
    uint32_t displayPtr = w2c_0x24pokeemerald0x2Ewasm_WasmDisplayBuffer(instance);
    return w2c_0x24pokeemerald0x2Ewasm_memory(instance)->data + displayPtr;
}

uint64_t native_engine_hash_display(const NativeEngine *engine)
{
    return hash_bytes64(native_engine_display_buffer(engine), NATIVE_DISPLAY_BYTES);
}

uint32_t native_engine_load_flash(NativeEngine *engine, const char *path)
{
    uint8_t *flash = flash_bytes(&engine->instance);
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
    uint8_t *flash = flash_bytes(&engine->instance);
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
    return w2c_0x24pokeemerald0x2Ewasm_WasmCheckSpriteSort(&engine->instance);
}
