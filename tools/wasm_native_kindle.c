#include "pokeemerald_wasm2c.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <dirent.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DISPLAY_WIDTH 240
#define DISPLAY_HEIGHT 160
#define REG_KEYINPUT 0x04000130u
#define KEY_MASK 0x03ffu
#define FLASH_BASE 0x0e000000u
#define FLASH_SIZE (128u * 1024u)
#define DEFAULT_SAVE_PATH "build/native/pokeemerald-kindle.sav"
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
#define BUTTON_EXIT   (1u << 10)

typedef w2c_0x24pokeemerald0x2Ewasm Pokeemerald;

struct w2c_env {
    Pokeemerald *instance;
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

#if defined(__linux__)

#define DEFAULT_FB_PATH "/dev/fb0"
#define DEFAULT_INPUT_PATH "/dev/input/event1"
#define DEFAULT_KINDLE_DISPLAY_FPS 4.0
#define MAX_INPUT_DEVICES 8
#define MAX_TOUCHES 8
#define MAX_UI_BUTTONS 11

#ifndef EVIOCGABS
#define EVIOCGABS(abs) _IOR('E', 0x40 + (abs), struct input_absinfo)
#endif
#ifndef EVIOCGRAB
#define EVIOCGRAB _IOW('E', 0x90, int)
#endif

#ifndef MXCFB_SEND_UPDATE
struct mxcfb_rect {
    uint32_t top;
    uint32_t left;
    uint32_t width;
    uint32_t height;
};

struct mxcfb_alt_buffer_data {
    uint32_t phys_addr;
    uint32_t width;
    uint32_t height;
    struct mxcfb_rect alt_update_region;
};

struct mxcfb_update_data {
    struct mxcfb_rect update_region;
    uint32_t waveform_mode;
    uint32_t update_mode;
    uint32_t update_marker;
    int32_t temp;
    uint32_t flags;
    struct mxcfb_alt_buffer_data alt_buffer_data;
};

#define MXCFB_SEND_UPDATE _IOW('F', 0x2E, struct mxcfb_update_data)
#endif

struct hwtcon_rect {
    uint32_t top;
    uint32_t left;
    uint32_t width;
    uint32_t height;
};

struct hwtcon_update_data {
    struct hwtcon_rect update_region;
    uint32_t waveform_mode;
    uint32_t update_mode;
    uint32_t update_marker;
    unsigned int flags;
    int dither_mode;
};

#define HWTCON_SEND_UPDATE 1076119086

#ifndef WAVEFORM_MODE_AUTO
#define WAVEFORM_MODE_AUTO 257u
#endif
#ifndef HWTCON_WAVEFORM_MODE_AUTO
#define HWTCON_WAVEFORM_MODE_AUTO 257u
#endif
#ifndef UPDATE_MODE_PARTIAL
#define UPDATE_MODE_PARTIAL 0u
#endif
#ifndef UPDATE_MODE_FULL
#define UPDATE_MODE_FULL 1u
#endif
#ifndef TEMP_USE_AMBIENT
#define TEMP_USE_AMBIENT 0x1000
#endif

typedef struct {
    int x;
    int y;
    int width;
    int height;
} RectI;

typedef struct {
    const char *label;
    RectI rect;
    uint32_t mask;
} KindleButton;

typedef struct {
    RectI screen;
    KindleButton buttons[MAX_UI_BUTTONS];
    size_t buttonCount;
} KindleLayout;

typedef struct {
    int fd;
    uint8_t *data;
    size_t size;
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    int width;
    int height;
    int bytesPerPixel;
    bool warnedRefresh;
    bool useHwtcon;
} Framebuffer;

typedef struct {
    int fd;
    int minX;
    int maxX;
    int minY;
    int maxY;
    int x;
    int y;
    bool down;
    bool grabbed;
    uint32_t keyHeld;
} InputDevice;

static volatile sig_atomic_t gQuit;

static void handle_signal(int signal)
{
    (void)signal;
    gQuit = 1;
}

static double monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void sleep_seconds(double seconds)
{
    if (seconds <= 0.0)
        return;

    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1000000000.0);
    nanosleep(&ts, NULL);
}

static bool open_framebuffer(Framebuffer *fb, const char *path)
{
    memset(fb, 0, sizeof(*fb));
    fb->fd = open(path, O_RDWR);
    if (fb->fd < 0) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return false;
    }

    if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->fix) < 0 || ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->var) < 0) {
        fprintf(stderr, "query %s: %s\n", path, strerror(errno));
        close(fb->fd);
        return false;
    }

    fb->width = (int)fb->var.xres;
    fb->height = (int)fb->var.yres;
    fb->bytesPerPixel = (int)((fb->var.bits_per_pixel + 7) / 8);
    fb->useHwtcon = strstr(fb->fix.id, "hwtcon") != NULL;
    fb->size = fb->fix.smem_len;
    fb->data = mmap(NULL, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    if (fb->data == MAP_FAILED) {
        fprintf(stderr, "mmap %s: %s\n", path, strerror(errno));
        close(fb->fd);
        return false;
    }

    return true;
}

static void close_framebuffer(Framebuffer *fb)
{
    if (fb->data && fb->data != MAP_FAILED)
        munmap(fb->data, fb->size);
    if (fb->fd >= 0)
        close(fb->fd);
}

static bool refresh_hwtcon(Framebuffer *fb, RectI region, bool full, int *error)
{
    struct hwtcon_update_data update;
    memset(&update, 0, sizeof(update));
    update.update_region.left = (uint32_t)region.x;
    update.update_region.top = (uint32_t)region.y;
    update.update_region.width = (uint32_t)region.width;
    update.update_region.height = (uint32_t)region.height;
    update.waveform_mode = HWTCON_WAVEFORM_MODE_AUTO;
    update.update_mode = full ? UPDATE_MODE_FULL : UPDATE_MODE_PARTIAL;

    if (ioctl(fb->fd, HWTCON_SEND_UPDATE, &update) == 0)
        return true;

    *error = errno;
    return false;
}

static bool refresh_mxcfb(Framebuffer *fb, RectI region, bool full, int *error)
{
    struct mxcfb_update_data update;
    memset(&update, 0, sizeof(update));
    update.update_region.left = (uint32_t)region.x;
    update.update_region.top = (uint32_t)region.y;
    update.update_region.width = (uint32_t)region.width;
    update.update_region.height = (uint32_t)region.height;
    update.waveform_mode = WAVEFORM_MODE_AUTO;
    update.update_mode = full ? UPDATE_MODE_FULL : UPDATE_MODE_PARTIAL;
    update.temp = TEMP_USE_AMBIENT;

    if (ioctl(fb->fd, MXCFB_SEND_UPDATE, &update) == 0)
        return true;

    *error = errno;
    return false;
}

static bool refresh_eips(Framebuffer *fb, bool full)
{
    char command[160];
    snprintf(command,
             sizeof(command),
             "/usr/sbin/eips -s w=%d,h=%d %s >/dev/null 2>&1",
             fb->width,
             fb->height,
             full ? "-f" : "");
    return system(command) == 0;
}

static void refresh_framebuffer(Framebuffer *fb, RectI region, bool full)
{
    int hwtconError = 0;
    int mxcfbError = 0;

    if (fb->useHwtcon) {
        if (refresh_hwtcon(fb, region, full, &hwtconError) || refresh_eips(fb, full))
            return;
    } else {
        if (refresh_mxcfb(fb, region, full, &mxcfbError) || refresh_eips(fb, full))
            return;
    }

    if (!fb->warnedRefresh) {
        fprintf(stderr,
                "warning: e-ink refresh failed: HWTCON=%s, MXCFB=%s; framebuffer writes may still be visible if auto-refresh is active\n",
                hwtconError ? strerror(hwtconError) : "not used",
                mxcfbError ? strerror(mxcfbError) : "not used");
        fb->warnedRefresh = true;
    }
}

static uint8_t rgba_to_gray(const uint8_t *rgba)
{
    return (uint8_t)(((uint32_t)rgba[0] * 77u + (uint32_t)rgba[1] * 150u + (uint32_t)rgba[2] * 29u) >> 8);
}

static void put_gray_pixel(Framebuffer *fb, int x, int y, uint8_t gray)
{
    if ((unsigned)x >= (unsigned)fb->width || (unsigned)y >= (unsigned)fb->height)
        return;

    uint8_t *pixel = fb->data + (size_t)(y + fb->var.yoffset) * fb->fix.line_length + (size_t)(x + fb->var.xoffset) * fb->bytesPerPixel;
    switch (fb->var.bits_per_pixel) {
    case 8:
        pixel[0] = gray;
        break;
    case 16: {
        uint16_t value = (uint16_t)(((gray >> 3) << 11) | ((gray >> 2) << 5) | (gray >> 3));
        pixel[0] = value & 0xff;
        pixel[1] = value >> 8;
        break;
    }
    case 24:
        pixel[0] = gray;
        pixel[1] = gray;
        pixel[2] = gray;
        break;
    case 32:
        pixel[0] = gray;
        pixel[1] = gray;
        pixel[2] = gray;
        pixel[3] = 0xff;
        break;
    default:
        break;
    }
}

static void fill_rect(Framebuffer *fb, RectI rect, uint8_t gray)
{
    int x0 = rect.x < 0 ? 0 : rect.x;
    int y0 = rect.y < 0 ? 0 : rect.y;
    int x1 = rect.x + rect.width > fb->width ? fb->width : rect.x + rect.width;
    int y1 = rect.y + rect.height > fb->height ? fb->height : rect.y + rect.height;

    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++)
            put_gray_pixel(fb, x, y, gray);
    }
}

static void draw_rect_outline(Framebuffer *fb, RectI rect, int thickness, uint8_t gray)
{
    fill_rect(fb, (RectI){rect.x, rect.y, rect.width, thickness}, gray);
    fill_rect(fb, (RectI){rect.x, rect.y + rect.height - thickness, rect.width, thickness}, gray);
    fill_rect(fb, (RectI){rect.x, rect.y, thickness, rect.height}, gray);
    fill_rect(fb, (RectI){rect.x + rect.width - thickness, rect.y, thickness, rect.height}, gray);
}

static uint8_t glyph_rows(char c, int row)
{
    static const uint8_t digits[10][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2},
        {31,16,30,1,1,17,14}, {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14}, {14,17,17,15,1,2,12},
    };
    static const uint8_t letters[26][7] = {
        {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30}, {31,16,16,30,16,16,31},
        {31,16,16,30,16,16,16}, {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17}, {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
        {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31}, {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17}, {14,17,17,17,17,17,14},
        {30,17,17,30,16,16,16}, {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17}, {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
        {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4}, {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17}, {17,17,10,4,4,4,4},
        {31,1,2,4,8,16,31},
    };

    if (c >= '0' && c <= '9')
        return digits[c - '0'][row];
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z')
        return letters[c - 'A'][row];
    return 0;
}

static void draw_text(Framebuffer *fb, const char *text, int x, int y, int scale, uint8_t gray)
{
    for (const char *p = text; *p; p++, x += 6 * scale) {
        if (*p == ' ')
            continue;
        for (int row = 0; row < 7; row++) {
            uint8_t bits = glyph_rows(*p, row);
            for (int col = 0; col < 5; col++) {
                if (bits & (1u << (4 - col)))
                    fill_rect(fb, (RectI){x + col * scale, y + row * scale, scale, scale}, gray);
            }
        }
    }
}

static int text_width(const char *text, int scale)
{
    return (int)strlen(text) * 6 * scale;
}

static void add_button(KindleLayout *layout, const char *label, RectI rect, uint32_t mask)
{
    if (layout->buttonCount < MAX_UI_BUTTONS)
        layout->buttons[layout->buttonCount++] = (KindleButton){ label, rect, mask };
}

static KindleLayout make_layout(int width, int height)
{
    KindleLayout layout;
    memset(&layout, 0, sizeof(layout));

    int margin = width / 24;
    if (margin < 24)
        margin = 24;

    int exitHeight = width / 30;
    if (exitHeight < 44)
        exitHeight = 44;
    int exitWidth = width / 4;
    if (exitWidth < 180)
        exitWidth = 180;
    int exitY = margin / 2;
    if (exitY < 8)
        exitY = 8;

    int controlsHeight = height / 3;
    if (controlsHeight < 360)
        controlsHeight = 360;

    int maxScreenWidth = width - margin * 2;
    int maxScreenHeight = height - controlsHeight - exitY - exitHeight - margin;
    if (maxScreenHeight < DISPLAY_HEIGHT)
        maxScreenHeight = height - margin * 2;

    int scaleX = maxScreenWidth / DISPLAY_WIDTH;
    int scaleY = maxScreenHeight / DISPLAY_HEIGHT;
    int scale = scaleX < scaleY ? scaleX : scaleY;
    if (scale < 1)
        scale = 1;

    layout.screen.width = DISPLAY_WIDTH * scale;
    layout.screen.height = DISPLAY_HEIGHT * scale;
    layout.screen.x = (width - layout.screen.width) / 2;
    layout.screen.y = exitY + exitHeight + margin / 2;

    add_button(&layout, "EXIT", (RectI){(width - exitWidth) / 2, exitY, exitWidth, exitHeight}, BUTTON_EXIT);

    int lrGap = margin;
    int lrHeight = exitHeight;
    int lrWidth = (layout.screen.width - lrGap) / 2;
    int lrY = layout.screen.y + layout.screen.height + margin / 3;
    add_button(&layout, "L", (RectI){layout.screen.x, lrY, lrWidth, lrHeight}, BUTTON_L);
    add_button(&layout, "R", (RectI){layout.screen.x + lrWidth + lrGap, lrY, lrWidth, lrHeight}, BUTTON_R);

    int bottomMargin = margin / 2;
    if (bottomMargin < 24)
        bottomMargin = 24;
    int bottomButtonHeight = exitHeight;
    int bottomY = height - bottomMargin - bottomButtonHeight;

    int controlsTop = lrY + lrHeight + margin / 2;
    int available = bottomY - controlsTop - margin / 2;
    int unit = available / 3;
    if (unit > width / 8)
        unit = width / 8;
    if (unit < 48)
        unit = 48;
    int pad = unit / 5;
    int button = unit - pad;
    int y = controlsTop;
    if (available > unit * 3)
        y += (available - unit * 3) / 2;

    int dpadX = margin;
    add_button(&layout, "UP", (RectI){dpadX + unit, y, button, button}, BUTTON_UP);
    add_button(&layout, "LEFT", (RectI){dpadX, y + unit, button, button}, BUTTON_LEFT);
    add_button(&layout, "RIGHT", (RectI){dpadX + unit * 2, y + unit, button, button}, BUTTON_RIGHT);
    add_button(&layout, "DOWN", (RectI){dpadX + unit, y + unit * 2, button, button}, BUTTON_DOWN);

    int faceX = width - margin - unit * 3;
    add_button(&layout, "B", (RectI){faceX, y + unit, button + unit / 3, button + unit / 3}, BUTTON_B);
    add_button(&layout, "A", (RectI){faceX + unit * 3 / 2, y + unit / 2, button + unit / 3, button + unit / 3}, BUTTON_A);

    int bottomButtonWidth = unit * 2;
    int bottomGap = margin / 2;
    int bottomX = (width - bottomButtonWidth * 2 - bottomGap) / 2;
    add_button(&layout, "SELECT", (RectI){bottomX, bottomY, bottomButtonWidth, bottomButtonHeight}, BUTTON_SELECT);
    add_button(&layout, "START", (RectI){bottomX + bottomButtonWidth + bottomGap, bottomY, bottomButtonWidth, bottomButtonHeight}, BUTTON_START);

    return layout;
}

static bool contains(RectI rect, int x, int y)
{
    return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
}

static uint32_t touch_buttons(const KindleLayout *layout, int x, int y, bool down)
{
    if (!down)
        return 0;

    uint32_t held = 0;
    for (size_t i = 0; i < layout->buttonCount; i++) {
        if (contains(layout->buttons[i].rect, x, y))
            held |= layout->buttons[i].mask;
    }
    return held;
}

static void draw_button(Framebuffer *fb, const KindleButton *button, uint32_t held)
{
    bool down = (held & button->mask) != 0;
    fill_rect(fb, button->rect, down ? 120 : 224);
    draw_rect_outline(fb, button->rect, 3, 24);

    int scale = button->rect.height >= 70 ? 3 : 2;
    int width = text_width(button->label, scale);
    int x = button->rect.x + (button->rect.width - width) / 2;
    int y = button->rect.y + (button->rect.height - 7 * scale) / 2;
    draw_text(fb, button->label, x, y, scale, 16);
}

static void draw_game(Framebuffer *fb, const KindleLayout *layout, const uint8_t *rgba)
{
    fill_rect(fb, (RectI){0, 0, fb->width, fb->height}, 255);
    fill_rect(fb, (RectI){layout->screen.x - 6, layout->screen.y - 6, layout->screen.width + 12, layout->screen.height + 12}, 0);

    int scale = layout->screen.width / DISPLAY_WIDTH;
    for (int sy = 0; sy < DISPLAY_HEIGHT; sy++) {
        for (int sx = 0; sx < DISPLAY_WIDTH; sx++) {
            uint8_t gray = rgba_to_gray(rgba + ((sy * DISPLAY_WIDTH + sx) * 4));
            fill_rect(fb, (RectI){layout->screen.x + sx * scale, layout->screen.y + sy * scale, scale, scale}, gray);
        }
    }
}

static void draw_ui(Framebuffer *fb, const KindleLayout *layout, uint32_t held)
{
    for (size_t i = 0; i < layout->buttonCount; i++)
        draw_button(fb, &layout->buttons[i], held);
}

static void set_abs_range(InputDevice *device, int code, int *minValue, int *maxValue)
{
    struct input_absinfo info;
    if (ioctl(device->fd, EVIOCGABS(code), &info) == 0 && info.maximum > info.minimum) {
        *minValue = info.minimum;
        *maxValue = info.maximum;
    }
}

static bool open_input_device(InputDevice *device, const char *path)
{
    memset(device, 0, sizeof(*device));
    device->fd = open(path, O_RDONLY | O_NONBLOCK);
    if (device->fd < 0)
        return false;

    device->minX = 0;
    device->minY = 0;
    device->maxX = 4095;
    device->maxY = 4095;
    set_abs_range(device, ABS_MT_POSITION_X, &device->minX, &device->maxX);
    set_abs_range(device, ABS_X, &device->minX, &device->maxX);
    set_abs_range(device, ABS_MT_POSITION_Y, &device->minY, &device->maxY);
    set_abs_range(device, ABS_Y, &device->minY, &device->maxY);

    int grab = 1;
    if (ioctl(device->fd, EVIOCGRAB, &grab) == 0) {
        device->grabbed = true;
    } else {
        fprintf(stderr, "warning: EVIOCGRAB %s failed: %s; touches may pass through to Kindle UI\n", path, strerror(errno));
    }

    return true;
}

static int scale_abs(int value, int minValue, int maxValue, int outSize)
{
    if (maxValue <= minValue)
        return value;

    if (value < minValue)
        value = minValue;
    if (value > maxValue)
        value = maxValue;
    return (int)(((int64_t)(value - minValue) * (outSize - 1)) / (maxValue - minValue));
}

static uint32_t key_mask(uint16_t code)
{
    switch (code) {
    case KEY_Z: return BUTTON_A;
    case KEY_X: return BUTTON_B;
    case KEY_ENTER: return BUTTON_START;
    case KEY_RIGHTSHIFT:
    case KEY_LEFTSHIFT: return BUTTON_SELECT;
    case KEY_RIGHT: return BUTTON_RIGHT;
    case KEY_LEFT: return BUTTON_LEFT;
    case KEY_UP: return BUTTON_UP;
    case KEY_DOWN: return BUTTON_DOWN;
    case KEY_S: return BUTTON_R;
    case KEY_A: return BUTTON_L;
    default: return 0;
    }
}

static void poll_input(InputDevice *devices, int deviceCount, int fbWidth, int fbHeight)
{
    for (int i = 0; i < deviceCount; i++) {
        struct input_event event;
        while (read(devices[i].fd, &event, sizeof(event)) == sizeof(event)) {
            if (event.type == EV_KEY) {
                if (event.code == BTN_TOUCH) {
                    devices[i].down = event.value != 0;
                } else {
                    uint32_t mask = key_mask(event.code);
                    if (mask) {
                        if (event.value)
                            devices[i].keyHeld |= mask;
                        else
                            devices[i].keyHeld &= ~mask;
                    }
                }
            } else if (event.type == EV_ABS) {
                if (event.code == ABS_MT_TRACKING_ID)
                    devices[i].down = event.value >= 0;
                else if (event.code == ABS_MT_POSITION_X || event.code == ABS_X)
                    devices[i].x = scale_abs(event.value, devices[i].minX, devices[i].maxX, fbWidth);
                else if (event.code == ABS_MT_POSITION_Y || event.code == ABS_Y)
                    devices[i].y = scale_abs(event.value, devices[i].minY, devices[i].maxY, fbHeight);
            }
        }
    }
}

static uint32_t input_buttons(InputDevice *devices, int deviceCount, const KindleLayout *layout)
{
    uint32_t held = 0;
    for (int i = 0; i < deviceCount; i++) {
        held |= devices[i].keyHeld;
        held |= touch_buttons(layout, devices[i].x, devices[i].y, devices[i].down);
    }
    return held;
}

static int open_inputs(InputDevice *devices, int maxDevices, const char *requested)
{
    if (requested) {
        if (open_input_device(&devices[0], requested))
            return 1;
        fprintf(stderr, "warning: open %s: %s\n", requested, strerror(errno));
        return 0;
    }

    int count = 0;
    for (int i = 0; i < 16 && count < maxDevices; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        if (open_input_device(&devices[count], path))
            count++;
    }
    return count;
}

static void close_inputs(InputDevice *devices, int deviceCount)
{
    for (int i = 0; i < deviceCount; i++) {
        if (devices[i].fd >= 0) {
            if (devices[i].grabbed) {
                int grab = 0;
                (void)ioctl(devices[i].fd, EVIOCGRAB, &grab);
            }
            close(devices[i].fd);
        }
    }
}

#endif

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

#if defined(__linux__)
int main(int argc, char **argv)
{
    const char *fbPath = DEFAULT_FB_PATH;
    const char *inputPath = NULL;
    const char *savePath = DEFAULT_SAVE_PATH;
    int frameLimit = 0;
    double displayFps = DEFAULT_KINDLE_DISPLAY_FPS;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fb") == 0 && i + 1 < argc) {
            fbPath = argv[++i];
        } else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            inputPath = argv[++i];
        } else if (strcmp(argv[i], "--save") == 0 && i + 1 < argc) {
            savePath = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frameLimit = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--display-fps") == 0 && i + 1 < argc) {
            displayFps = strtod(argv[++i], NULL);
            if (displayFps <= 0.0)
                displayFps = DEFAULT_KINDLE_DISPLAY_FPS;
        }
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    Framebuffer fb;
    if (!open_framebuffer(&fb, fbPath))
        return 1;

    InputDevice inputs[MAX_INPUT_DEVICES];
    int inputCount = open_inputs(inputs, MAX_INPUT_DEVICES, inputPath);
    if (inputCount == 0)
        fprintf(stderr, "warning: no input devices opened; use --input /dev/input/eventN if touch is unavailable\n");

    wasm_rt_init();
    Pokeemerald instance;
    memset(&instance, 0, sizeof(instance));
    struct w2c_env env = { .instance = &instance };
    wasm2c_0x24pokeemerald0x2Ewasm_instantiate(&instance, &env);

    uint32_t lastSaveHash = load_flash(&instance, savePath);
    write_keys(&instance, 0);
    w2c_0x24pokeemerald0x2Ewasm_AgbMain(&instance);

    KindleLayout layout = make_layout(fb.width, fb.height);
    double lastFrameTime = monotonic_seconds();
    double frameAccumulator = 0.0;
    double displayInterval = 1.0 / displayFps;
    double nextDisplay = lastFrameTime;
    uint32_t frame = 0;

    while (!gQuit) {
        double now = monotonic_seconds();
        double elapsed = now - lastFrameTime;
        lastFrameTime = now;
        frameAccumulator += elapsed * 60.0;

        poll_input(inputs, inputCount, fb.width, fb.height);
        uint32_t held = input_buttons(inputs, inputCount, &layout);
        if (held & BUTTON_EXIT)
            gQuit = 1;
        write_keys(&instance, held);

        int framesToRun = (int)frameAccumulator;
        if (framesToRun > 8)
            framesToRun = 8;
        if (frameLimit > 0 && frame + (uint32_t)framesToRun > (uint32_t)frameLimit)
            framesToRun = (int)((uint32_t)frameLimit - frame);
        frameAccumulator -= framesToRun;

        for (int i = 0; i < framesToRun; i++) {
            w2c_0x24pokeemerald0x2Ewasm_WasmRunFrame(&instance);
            frame++;
        }

        now = monotonic_seconds();
        if (now >= nextDisplay || frameLimit > 0) {
            w2c_0x24pokeemerald0x2Ewasm_WasmRenderFrame(&instance);
            uint32_t displayPtr = w2c_0x24pokeemerald0x2Ewasm_WasmDisplayBuffer(&instance);
            const uint8_t *display = w2c_0x24pokeemerald0x2Ewasm_memory(&instance)->data + displayPtr;
            draw_game(&fb, &layout, display);
            draw_ui(&fb, &layout, held);
            refresh_framebuffer(&fb, (RectI){0, 0, fb.width, fb.height}, false);
            nextDisplay = now + displayInterval;
        }

        if (framesToRun > 0 && frame % SAVE_FLUSH_FRAMES == 0)
            lastSaveHash = save_flash_if_changed(&instance, savePath, lastSaveHash, false);
        if (frameLimit > 0 && frame >= (uint32_t)frameLimit)
            break;

        double sleepFor = nextDisplay - monotonic_seconds();
        if (sleepFor > 0.002)
            sleep_seconds(sleepFor < 0.025 ? sleepFor : 0.025);
    }

    lastSaveHash = save_flash_if_changed(&instance, savePath, lastSaveHash, true);
    (void)lastSaveHash;
    wasm2c_0x24pokeemerald0x2Ewasm_free(&instance);
    wasm_rt_free();
    close_inputs(inputs, inputCount);
    close_framebuffer(&fb);
    return 0;
}
#else
int main(void)
{
    fprintf(stderr, "pokeemerald Kindle frontend requires Linux fbdev/input at runtime. Cross-compile this target for Kindle Scribe Linux.\n");
    return 1;
}
#endif
