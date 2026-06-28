#include "raylib.h"
#include "pokeemerald_wasm2c.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DISPLAY_WIDTH 240
#define DISPLAY_HEIGHT 160
#define REG_KEYINPUT 0x04000130u
#define KEY_MASK 0x03ffu
#define FLASH_BASE 0x0e000000u
#define FLASH_SIZE (128u * 1024u)
#define DEFAULT_SAVE_PATH "build/native/pokeemerald-native.sav"
#define SAVE_FLUSH_FRAMES 60
#define DISPLAY_FPS 60
#define MIN_DISPLAY_FPS 6
#define MAX_INTERNAL_FRAME_SECONDS (1.0 / MIN_DISPLAY_FPS)

#define BUTTON_A      (1u << 0)
#define BUTTON_B      (1u << 1)
#define BUTTON_SELECT (1u << 2)
#define BUTTON_START  (1u << 3)
#define BUTTON_RIGHT  (1u << 4)
#define BUTTON_LEFT   (1u << 5)
#define BUTTON_UP     (1u << 6)
#define BUTTON_DOWN   (1u << 7)
#define BUTTON_R      (1u << 8)
#define BUTTON_L      (1u << 9)

typedef w2c_0x24pokeemerald0x2Ewasm Pokeemerald;

struct w2c_env {
    Pokeemerald *instance;
};

typedef struct {
    const char *label;
    Rectangle rect;
    uint32_t mask;
} UiButton;

typedef enum {
    SPEED_HALVE,
    SPEED_RESET,
    SPEED_DOUBLE,
} SpeedAction;

typedef struct {
    const char *label;
    Rectangle rect;
    SpeedAction action;
} SpeedButton;

typedef struct {
    Rectangle screen;
    UiButton buttons[10];
    size_t buttonCount;
    SpeedButton speedButtons[4];
    size_t speedButtonCount;
} UiLayout;

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
    for (uint32_t i = 0; i < count; i++) {
        uint32_t s = src + i * 6;
        uint32_t d = dst + i * offset * 4;
        int16_t xScale = read_s16(env, s);
        int16_t yScale = read_s16(env, s + 2);
        uint16_t rotation = read_u16(env, s + 4);
        double angle = rotation * M_PI * 2.0 / 0x10000;
        double sn = sin(angle) * 256.0;
        double cs = cos(angle) * 256.0;
        write_s16(env, d, (int32_t)(cs * xScale / 256.0));
        write_s16(env, d + offset, (int32_t)(-sn * xScale / 256.0));
        write_s16(env, d + offset * 2, (int32_t)(sn * yScale / 256.0));
        write_s16(env, d + offset * 3, (int32_t)(cs * yScale / 256.0));
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

static void ensure_save_dir(void)
{
    mkdir("build", 0777);
    mkdir("build/native", 0777);
}

static uint8_t *flash_bytes(Pokeemerald *instance)
{
    return w2c_0x24pokeemerald0x2Ewasm_memory(instance)->data + FLASH_BASE;
}

static uint32_t load_flash(Pokeemerald *instance, const char *path)
{
    uint8_t *flash = flash_bytes(instance);
    memset(flash, 0xff, FLASH_SIZE);

    FILE *file = fopen(path, "rb");
    if (file) {
        fseek(file, 0, SEEK_END);
        long size = ftell(file);
        rewind(file);
        if (size == FLASH_SIZE)
            (void)fread(flash, 1, FLASH_SIZE, file);
        fclose(file);
    }

    return hash_bytes(flash, FLASH_SIZE);
}

static uint32_t save_flash_if_changed(Pokeemerald *instance, const char *path, uint32_t lastHash, bool force)
{
    uint8_t *flash = flash_bytes(instance);
    uint32_t hash = hash_bytes(flash, FLASH_SIZE);
    if (!force && hash == lastHash)
        return lastHash;

    ensure_save_dir();
    FILE *file = fopen(path, "wb");
    if (file) {
        (void)fwrite(flash, 1, FLASH_SIZE, file);
        fclose(file);
        return hash;
    }

    return lastHash;
}

static void write_keys(Pokeemerald *instance, uint32_t held)
{
    uint8_t *mem = w2c_0x24pokeemerald0x2Ewasm_memory(instance)->data;
    uint16_t value = KEY_MASK ^ (held & KEY_MASK);
    mem[REG_KEYINPUT] = value & 0xff;
    mem[REG_KEYINPUT + 1] = value >> 8;
}

static bool mouse_button_down(Rectangle rect)
{
    return IsMouseButtonDown(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), rect);
}

static uint32_t keyboard_buttons(void)
{
    uint32_t held = 0;
    if (IsKeyDown(KEY_Z)) held |= BUTTON_A;
    if (IsKeyDown(KEY_X)) held |= BUTTON_B;
    if (IsKeyDown(KEY_ENTER)) held |= BUTTON_START;
    if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) held |= BUTTON_SELECT;
    if (IsKeyDown(KEY_RIGHT)) held |= BUTTON_RIGHT;
    if (IsKeyDown(KEY_LEFT)) held |= BUTTON_LEFT;
    if (IsKeyDown(KEY_UP)) held |= BUTTON_UP;
    if (IsKeyDown(KEY_DOWN)) held |= BUTTON_DOWN;
    if (IsKeyDown(KEY_S)) held |= BUTTON_R;
    if (IsKeyDown(KEY_A)) held |= BUTTON_L;
    return held;
}

static void add_button(UiLayout *layout, const char *label, Rectangle rect, uint32_t mask)
{
    layout->buttons[layout->buttonCount++] = (UiButton){ label, rect, mask };
}

static void add_speed_button(UiLayout *layout, const char *label, Rectangle rect, SpeedAction action)
{
    layout->speedButtons[layout->speedButtonCount++] = (SpeedButton){ label, rect, action };
}

static UiLayout make_layout(void)
{
    UiLayout layout = {0};
    const int width = GetScreenWidth();
    const int height = GetScreenHeight();
    const float screenAreaHeight = height - 210.0f;
    float scale = fminf((width - 40.0f) / DISPLAY_WIDTH, screenAreaHeight / DISPLAY_HEIGHT);
    if (scale < 1.0f)
        scale = 1.0f;

    layout.screen = (Rectangle){
        floorf((width - DISPLAY_WIDTH * scale) * 0.5f),
        20.0f,
        DISPLAY_WIDTH * scale,
        DISPLAY_HEIGHT * scale,
    };

    const float y = layout.screen.y + layout.screen.height + 30.0f;
    const float unit = 50.0f;
    const float dpadX = 50.0f;
    add_button(&layout, "UP", (Rectangle){dpadX + unit, y, unit, unit}, BUTTON_UP);
    add_button(&layout, "LEFT", (Rectangle){dpadX, y + unit, unit, unit}, BUTTON_LEFT);
    add_button(&layout, "RIGHT", (Rectangle){dpadX + unit * 2, y + unit, unit, unit}, BUTTON_RIGHT);
    add_button(&layout, "DOWN", (Rectangle){dpadX + unit, y + unit * 2, unit, unit}, BUTTON_DOWN);

    const float faceX = width - 230.0f;
    add_button(&layout, "B", (Rectangle){faceX, y + unit, 70.0f, 70.0f}, BUTTON_B);
    add_button(&layout, "A", (Rectangle){faceX + 90.0f, y + 30.0f, 70.0f, 70.0f}, BUTTON_A);
    add_button(&layout, "L", (Rectangle){150.0f, y - 20.0f, 95.0f, 34.0f}, BUTTON_L);
    add_button(&layout, "R", (Rectangle){width - 245.0f, y - 20.0f, 95.0f, 34.0f}, BUTTON_R);
    add_button(&layout, "SELECT", (Rectangle){width * 0.5f - 110.0f, y + unit * 2.2f, 95.0f, 34.0f}, BUTTON_SELECT);
    add_button(&layout, "START", (Rectangle){width * 0.5f + 15.0f, y + unit * 2.2f, 95.0f, 34.0f}, BUTTON_START);

    const float speedY = y + unit * 0.35f;
    const float speedX = width * 0.5f - 160.0f;
    add_speed_button(&layout, "0.5x", (Rectangle){speedX, speedY, 96.0f, 34.0f}, SPEED_HALVE);
    add_speed_button(&layout, "Reset", (Rectangle){speedX + 112.0f, speedY, 84.0f, 34.0f}, SPEED_RESET);
    add_speed_button(&layout, "2x", (Rectangle){speedX + 212.0f, speedY, 96.0f, 34.0f}, SPEED_DOUBLE);
    return layout;
}

static uint32_t ui_buttons(const UiLayout *layout)
{
    uint32_t held = 0;
    for (size_t i = 0; i < layout->buttonCount; i++) {
        if (mouse_button_down(layout->buttons[i].rect))
            held |= layout->buttons[i].mask;
    }
    return held;
}

static void handle_speed_buttons(const UiLayout *layout, float *speed, float *frameAccumulator)
{
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        return;

    Vector2 mouse = GetMousePosition();
    for (size_t i = 0; i < layout->speedButtonCount; i++) {
        if (!CheckCollisionPointRec(mouse, layout->speedButtons[i].rect))
            continue;

        switch (layout->speedButtons[i].action) {
        case SPEED_HALVE:
            *speed *= 0.5f;
            break;
        case SPEED_RESET:
            *speed = 1.0f;
            break;
        case SPEED_DOUBLE:
            *speed *= 2.0f;
            break;
        }
        *frameAccumulator = 0.0f;
        return;
    }
}

static int frames_for_speed(float speed, double elapsedSeconds, float *frameAccumulator)
{
    *frameAccumulator += speed * (float)elapsedSeconds * DISPLAY_FPS;
    int frames = (int)floorf(*frameAccumulator);
    *frameAccumulator -= frames;
    return frames;
}

static void draw_button(const UiButton *button, uint32_t held)
{
    bool down = (held & button->mask) != 0;
    Color fill = down ? (Color){70, 190, 110, 255} : (Color){44, 49, 64, 255};
    DrawRectangleRounded(button->rect, 0.24f, 12, fill);
    DrawRectangleRoundedLines(button->rect, 0.24f, 12, down ? GREEN : GRAY);
    int textWidth = MeasureText(button->label, 20);
    DrawText(button->label,
             (int)(button->rect.x + (button->rect.width - textWidth) * 0.5f),
             (int)(button->rect.y + (button->rect.height - 20) * 0.5f),
             20,
             RAYWHITE);
}

static void draw_speed_button(const SpeedButton *button, float speed)
{
    bool active = button->action == SPEED_RESET && fabsf(speed - 1.0f) < 0.01f;
    Color fill = active ? (Color){70, 120, 205, 255} : (Color){44, 49, 64, 255};
    DrawRectangleRounded(button->rect, 0.22f, 12, fill);
    DrawRectangleRoundedLines(button->rect, 0.22f, 12, active ? SKYBLUE : GRAY);
    int textWidth = MeasureText(button->label, 18);
    DrawText(button->label,
             (int)(button->rect.x + (button->rect.width - textWidth) * 0.5f),
             (int)(button->rect.y + (button->rect.height - 18) * 0.5f),
             18,
             RAYWHITE);
}

static void draw_ui(Texture2D texture, const UiLayout *layout, uint32_t held, float speed, int internalFps, int displayFps)
{
    BeginDrawing();
    ClearBackground((Color){18, 20, 28, 255});
    DrawRectangleRounded((Rectangle){layout->screen.x - 8, layout->screen.y - 8, layout->screen.width + 16, layout->screen.height + 16}, 0.03f, 8, BLACK);
    DrawTexturePro(texture,
                   (Rectangle){0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT},
                   layout->screen,
                   (Vector2){0, 0},
                   0.0f,
                   WHITE);
    const char *fpsText = TextFormat("Internal FPS: %d   Display FPS: %d", internalFps, displayFps);
    DrawText(fpsText,
             (int)(layout->screen.x + (layout->screen.width - MeasureText(fpsText, 18)) * 0.5f),
             (int)(layout->screen.y + layout->screen.height + 8.0f),
             18,
             RAYWHITE);

    const char *speedText = TextFormat("Speed: %.3gx", speed);
    DrawText(speedText,
             (int)(GetScreenWidth() * 0.5f - MeasureText(speedText, 18) * 0.5f),
             (int)(layout->speedButtons[0].rect.y - 24),
             18,
             GRAY);
    for (size_t i = 0; i < layout->buttonCount; i++)
        draw_button(&layout->buttons[i], held);
    for (size_t i = 0; i < layout->speedButtonCount; i++)
        draw_speed_button(&layout->speedButtons[i], speed);
    EndDrawing();
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

int main(int argc, char **argv)
{
    const char *savePath = DEFAULT_SAVE_PATH;
    int frameLimit = 0;
    float initialSpeed = 1.0f;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--save") == 0 && i + 1 < argc) {
            savePath = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frameLimit = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--speed") == 0 && i + 1 < argc) {
            initialSpeed = strtof(argv[++i], NULL);
        }
    }

    wasm_rt_init();

    Pokeemerald instance;
    memset(&instance, 0, sizeof(instance));
    struct w2c_env env = { .instance = &instance };
    wasm2c_0x24pokeemerald0x2Ewasm_instantiate(&instance, &env);

    uint32_t lastSaveHash = load_flash(&instance, savePath);
    write_keys(&instance, 0);
    w2c_0x24pokeemerald0x2Ewasm_AgbMain(&instance);

    InitWindow(960, 720, "pokeemerald wasm2c native");
    SetWindowState(FLAG_WINDOW_RESIZABLE);
    SetTargetFPS(DISPLAY_FPS);

    Image image = GenImageColor(DISPLAY_WIDTH, DISPLAY_HEIGHT, BLACK);
    Texture2D texture = LoadTextureFromImage(image);
    UnloadImage(image);
    SetTextureFilter(texture, TEXTURE_FILTER_POINT);

    uint32_t frame = 0;
    float speed = initialSpeed > 0.0f ? initialSpeed : 1.0f;
    float frameAccumulator = 0.0f;
    double lastFrameTime = GetTime();
    double lastFpsTime = lastFrameTime;
    int internalFramesThisSecond = 0;
    int displayFramesThisSecond = 0;
    int internalFps = 0;
    int displayFps = 0;
    while (!WindowShouldClose()) {
        double frameNow = GetTime();
        double elapsedSeconds = frameNow - lastFrameTime;
        lastFrameTime = frameNow;

        UiLayout layout = make_layout();
        handle_speed_buttons(&layout, &speed, &frameAccumulator);
        uint32_t held = keyboard_buttons() | ui_buttons(&layout);
        int framesToRun = frames_for_speed(speed, elapsedSeconds, &frameAccumulator);
        if (frameLimit > 0 && frame + (uint32_t)framesToRun > (uint32_t)frameLimit)
            framesToRun = (int)((uint32_t)frameLimit - frame);

        int framesRun = 0;
        double internalStart = GetTime();
        if (framesToRun > 0)
            write_keys(&instance, held);
        while (framesRun < framesToRun) {
            w2c_0x24pokeemerald0x2Ewasm_WasmRunFrame(&instance);
            frame++;
            internalFramesThisSecond++;
            framesRun++;
            if (framesRun < framesToRun && (framesRun & 15) == 0 && GetTime() - internalStart >= MAX_INTERNAL_FRAME_SECONDS)
                break;
        }
        if (framesRun < framesToRun && frameLimit == 0)
            frameAccumulator += framesToRun - framesRun;
        w2c_0x24pokeemerald0x2Ewasm_WasmRenderFrame(&instance);

        displayFramesThisSecond++;
        double now = GetTime();
        double fpsElapsed = now - lastFpsTime;
        if (fpsElapsed >= 1.0) {
            internalFps = (int)round(internalFramesThisSecond / fpsElapsed);
            displayFps = (int)round(displayFramesThisSecond / fpsElapsed);
            internalFramesThisSecond = 0;
            displayFramesThisSecond = 0;
            lastFpsTime = now;
        }

        uint32_t displayPtr = w2c_0x24pokeemerald0x2Ewasm_WasmDisplayBuffer(&instance);
        UpdateTexture(texture, w2c_0x24pokeemerald0x2Ewasm_memory(&instance)->data + displayPtr);
        draw_ui(texture, &layout, held, speed, internalFps, displayFps);

        if (framesToRun > 0 && frame % SAVE_FLUSH_FRAMES == 0)
            lastSaveHash = save_flash_if_changed(&instance, savePath, lastSaveHash, false);
        if (frameLimit > 0 && frame >= (uint32_t)frameLimit)
            break;
    }

    lastSaveHash = save_flash_if_changed(&instance, savePath, lastSaveHash, true);
    (void)lastSaveHash;
    UnloadTexture(texture);
    CloseWindow();
    wasm2c_0x24pokeemerald0x2Ewasm_free(&instance);
    wasm_rt_free();
    return 0;
}
