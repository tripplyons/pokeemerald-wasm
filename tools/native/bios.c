#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
static uint16_t read_u16(uintptr_t addr)
{
    return (uint16_t)((*(uint8_t *)(addr)) | ((*(uint8_t *)(addr + 1)) << 8));
}

static int16_t read_s16(uintptr_t addr)
{
    return (int16_t)read_u16(addr);
}

static uint32_t read_u32(uintptr_t addr)
{
    return (uint32_t)(*(uint8_t *)(addr))
        | ((uint32_t)(*(uint8_t *)(addr + 1)) << 8)
        | ((uint32_t)(*(uint8_t *)(addr + 2)) << 16)
        | ((uint32_t)(*(uint8_t *)(addr + 3)) << 24);
}

static int32_t read_s32(uintptr_t addr)
{
    return (int32_t)read_u32(addr);
}

static void write_u16(uintptr_t addr, uint16_t value)
{
    (*(uint8_t *)(addr)) = value & 0xff;
    (*(uint8_t *)(addr + 1)) = value >> 8;
}

static void write_u32(uintptr_t addr, uint32_t value)
{
    (*(uint8_t *)(addr)) = value & 0xff;
    (*(uint8_t *)(addr + 1)) = (value >> 8) & 0xff;
    (*(uint8_t *)(addr + 2)) = (value >> 16) & 0xff;
    (*(uint8_t *)(addr + 3)) = value >> 24;
}

static void write_s16(uintptr_t addr, int32_t value)
{
    write_u16(addr, (uint16_t)value);
}

static void copy_units(uintptr_t src, uintptr_t dst, uint32_t count, uint32_t size, bool fill)
{


    for (uint32_t i = 0; i < count; i++) {
        uintptr_t from = fill ? src : src + i * size;
        memmove((uint8_t *)dst + i * size, (uint8_t *)from, size);
    }
}

static void lz77(uintptr_t src, uintptr_t dst)
{
    uint32_t size = (*(uint8_t *)(src + 1)) | ((*(uint8_t *)(src + 2)) << 8) | ((*(uint8_t *)(src + 3)) << 16);
    uintptr_t s = src + 4;
    uintptr_t d = dst;
    uintptr_t end = dst + size;


    while (d < end) {
        uint8_t flags = (*(uint8_t *)(s++));
        for (int bit = 7; bit >= 0 && d < end; bit--) {
            if (flags & (1 << bit)) {
                uint32_t pair = ((uint32_t)(*(uint8_t *)(s)) << 8) | (*(uint8_t *)(s + 1));
                s += 2;
                uint32_t length = (pair >> 12) + 3;
                uint32_t disp = (pair & 0xfff) + 1;
                while (length-- && d < end) {
                    (*(uint8_t *)(d)) = (*(uint8_t *)(d - disp));
                    d++;
                }
            } else {
                (*(uint8_t *)(d++)) = (*(uint8_t *)(s++));
            }
        }
    }
}

static void rl(uintptr_t src, uintptr_t dst)
{
    uint32_t size = (*(uint8_t *)(src + 1)) | ((*(uint8_t *)(src + 2)) << 8) | ((*(uint8_t *)(src + 3)) << 16);
    uintptr_t s = src + 4;
    uintptr_t d = dst;
    uintptr_t end = dst + size;


    while (d < end) {
        uint8_t flag = (*(uint8_t *)(s++));
        if (flag & 0x80) {
            uint32_t count = (flag & 0x7f) + 3;
            uint8_t value = (*(uint8_t *)(s++));
            while (count-- && d < end)
                (*(uint8_t *)(d++)) = value;
        } else {
            uint32_t count = (flag & 0x7f) + 1;
            while (count-- && d < end)
                (*(uint8_t *)(d++)) = (*(uint8_t *)(s++));
        }
    }
}

static void bg_affine_set(uintptr_t src, uintptr_t dst, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        uintptr_t s = src + i * 20;
        uintptr_t d = dst + i * 16;
        int32_t texX = read_s32(s);
        int32_t texY = read_s32(s + 4);
        int16_t scrX = read_s16(s + 8);
        int16_t scrY = read_s16(s + 10);
        int16_t xScale = read_s16(s + 12);
        int16_t yScale = read_s16(s + 14);
        uint16_t rotation = read_u16(s + 16);
        double angle = rotation * M_PI * 2.0 / 0x10000;
        double sn = sin(angle) * 256.0;
        double cs = cos(angle) * 256.0;
        int32_t a = (int32_t)(cs * xScale / 256.0);
        int32_t b = (int32_t)(-sn * xScale / 256.0);
        int32_t c = (int32_t)(sn * yScale / 256.0);
        int32_t e = (int32_t)(cs * yScale / 256.0);
        write_s16(d, a);
        write_s16(d + 2, b);
        write_s16(d + 4, c);
        write_s16(d + 6, e);
        write_u32(d + 8, (uint32_t)(texX - scrX * a - scrY * b));
        write_u32(d + 12, (uint32_t)(texY - scrX * c - scrY * e));
    }
}

static void obj_affine_set(uintptr_t src, uintptr_t dst, uint32_t count, uint32_t offset)
{
    for (uint32_t i = 0; i < count; i++) {
        uintptr_t s = src + i * 6;
        uintptr_t d = dst + i * offset * 4;
        int16_t xScale = read_s16(s);
        int16_t yScale = read_s16(s + 2);
        uint16_t rotation = read_u16(s + 4);
        double angle = rotation * M_PI * 2.0 / 0x10000;
        double sn = sin(angle) * 256.0;
        double cs = cos(angle) * 256.0;
        write_s16(d, (int32_t)(cs * xScale / 256.0));
        write_s16(d + offset, (int32_t)(-sn * xScale / 256.0));
        write_s16(d + offset * 2, (int32_t)(sn * yScale / 256.0));
        write_s16(d + offset * 3, (int32_t)(cs * yScale / 256.0));
    }
}

static void copy_oam_matrices(uintptr_t src, uintptr_t dest,
                              uintptr_t dummy, uint32_t oam_count, uint32_t oam_limit)
{
    uint8_t *restrict source;
    uint8_t *restrict output;
    uint64_t dummy_oam;

    source = (uint8_t *)src;
    output = (uint8_t *)dest;
    memcpy(&dummy_oam, (uint8_t *)dummy, sizeof(dummy_oam));
    for (uint32_t i = oam_count; i < oam_limit; i++)
        memcpy(output + i * 8, &dummy_oam, sizeof(dummy_oam));

    output += 6;
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

uint32_t ArcTan2(uint32_t x, uint32_t y)
{
    double angle = atan2((double)(int32_t)y, (double)(int32_t)x);
    if (angle < 0.0)
        angle += M_PI * 2.0;
    return (uint32_t)(angle * 65536.0 / (M_PI * 2.0));
}

void BgAffineSet(uintptr_t src, uintptr_t dest, uint32_t count) { bg_affine_set(src, dest, count); }
void CpuFastSet(uintptr_t src, uintptr_t dest, uint32_t mode) { copy_units(src, dest, mode & 0x1fffff, 4, (mode >> 24) & 1); }
void CpuSet(uintptr_t src, uintptr_t dest, uint32_t mode) { copy_units(src, dest, mode & 0x1fffff, ((mode >> 26) & 1) ? 4 : 2, (mode >> 24) & 1); }
uint32_t Div(uint32_t num, uint32_t den) { return den ? (uint32_t)((int32_t)num / (int32_t)den) : 0; }
void LZ77UnCompVram(uintptr_t src, uintptr_t dest) { lz77(src, dest); }
void LZ77UnCompWram(uintptr_t src, uintptr_t dest) { lz77(src, dest); }
void ObjAffineSet(uintptr_t src, uintptr_t dest, uint32_t count, uint32_t offset) { obj_affine_set(src, dest, count, offset); }
void WasmCopyOamMatrices(uintptr_t src, uintptr_t dest, uintptr_t dummy, uint32_t count, uint32_t limit) { copy_oam_matrices(src, dest, dummy, count, limit); }
void RLUnCompVram(uintptr_t src, uintptr_t dest) { rl(src, dest); }
void RLUnCompWram(uintptr_t src, uintptr_t dest) { rl(src, dest); }
uint32_t Sqrt(uint32_t value) { return (uint32_t)sqrt((double)value); }

void FadeOutBody(uint32_t a) { (void)a; }
void GameCubeMultiBoot_ExecuteProgram(uint32_t a) { (void)a; }
void GameCubeMultiBoot_HandleSerialInterrupt(uint32_t a) { (void)a; }
void GameCubeMultiBoot_Init(uint32_t a) { (void)a; }
void GameCubeMultiBoot_Main(uint32_t a) { (void)a; }
void GameCubeMultiBoot_Quit(void) { }
uint32_t IsPokemonCryPlaying(uint32_t a) { (void)a; return 0; }
uint32_t MultiBoot(uint32_t a) { (void)a; return 0; }
void RealClearChain(uint32_t a) { (void)a; }
void RegisterRamReset(uint32_t a) { (void)a; }
void SampleFreqSet(uint32_t a) { (void)a; }
void SetPokemonCryChorus(uint32_t a) { (void)a; }
void SetPokemonCryLength(uint32_t a) { (void)a; }
void SetPokemonCryPanpot(uint32_t a) { (void)a; }
void SetPokemonCryPitch(uint32_t a) { (void)a; }
void SetPokemonCryProgress(uint32_t a) { (void)a; }
void SetPokemonCryRelease(uint32_t a) { (void)a; }
void SetPokemonCryStereo(uint32_t a) { (void)a; }
uint32_t SetPokemonCryTone(uint32_t a) { (void)a; return 0; }
void SetPokemonCryVolume(uint32_t a) { (void)a; }
void SoftReset(uint32_t a) { (void)a; }
void TrackStop(uint32_t a, uint32_t b) { (void)a; (void)b; }
void TrkVolPitSet(uint32_t a, uint32_t b) { (void)a; (void)b; }
void VBlankIntrWait(void) { }

#define NOOP_PLY(name) void name(uint32_t a, uint32_t b) { (void)a; (void)b; }
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
