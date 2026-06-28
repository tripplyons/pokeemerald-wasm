#if WASM

#include "global.h"
#include "gba/defines.h"
#include "gba/io_reg.h"

#define DISPLAY_WIDTH 240
#define DISPLAY_HEIGHT 160
#define DISPLAY_PIXELS (DISPLAY_WIDTH * DISPLAY_HEIGHT)
#define RGBA_CHANNELS 4

#define LAYER_BG0 0x01
#define LAYER_BG1 0x02
#define LAYER_BG2 0x04
#define LAYER_BG3 0x08
#define LAYER_OBJ 0x10
#define LAYER_BACKDROP 0x20
#define WINDOW_ALL_LAYERS 0x3f

#define REG_OFFSET_DMA0 0xb0
#define DMA_REG_SIZE 12
#define DMA_DEST_MASK 0x0060
#define DMA_DEST_FIXED 0x0040
#define DMA_DEST_RELOAD 0x0060
#define DMA_SRC_MASK 0x0180
#define DMA_SRC_DEC 0x0080
#define DMA_SRC_FIXED 0x0100
#define DMA_REPEAT 0x0200
#define DMA_32BIT 0x0400
#define DMA_START_HBLANK 0x2000
#define DMA_START_MASK 0x3000
#define DMA_ENABLE 0x8000
#define GPU_REG_U16_COUNT (REG_OFFSET_DMA0 / 2)

struct Rgb
{
    u8 r;
    u8 g;
    u8 b;
};

struct HblankDmaGpuReg
{
    bool8 active;
    u32 src;
    s32 stride;
};

struct BgLayer
{
    u8 bg;
    u8 type;
};

static u8 sWasmDisplayRgba[DISPLAY_PIXELS * RGBA_CHANNELS];
static u8 sLayerData[DISPLAY_PIXELS];
static struct HblankDmaGpuReg sHblankDmaGpuRegs[GPU_REG_U16_COUNT];

static inline u8 *Ptr8(u32 address)
{
    return (u8 *)address;
}

static inline u16 *Ptr16(u32 address)
{
    return (u16 *)address;
}

static inline u16 ReadU16(u32 address)
{
    return *Ptr16(address);
}

static inline u32 ReadU32(u32 address)
{
    return ((u32)ReadU16(address)) | ((u32)ReadU16(address + 2) << 16);
}

static inline s16 Signed16(u16 value)
{
    return (s16)value;
}

static inline s32 Signed28(u32 value)
{
    return ((s32)(value << 4)) >> 4;
}

static inline u32 Word(u32 offset)
{
    return ReadU32(REG_BASE + offset);
}

static inline u8 ClampBlend(u32 value)
{
    return value > 255 ? 255 : value;
}

static inline struct Rgb GbaColor(u16 value)
{
    struct Rgb color;
    color.r = (u8)((value & 31) * 255 / 31);
    color.g = (u8)(((value >> 5) & 31) * 255 / 31);
    color.b = (u8)(((value >> 10) & 31) * 255 / 31);
    return color;
}

static bool8 InWindowRange(u8 value, u16 range)
{
    const u8 start = range >> 8;
    const u8 end = range & 0xff;

    return start <= end ? value >= start && value < end : value >= start || value < end;
}

static void RefreshHblankDmaGpuRegs(void)
{
    u32 i;

    for (i = 0; i < GPU_REG_U16_COUNT; i++)
        sHblankDmaGpuRegs[i].active = FALSE;

    for (u32 channel = 0; channel < 4; channel++)
    {
        const u32 dma = REG_BASE + REG_OFFSET_DMA0 + channel * DMA_REG_SIZE;
        const u16 control = ReadU16(dma + 10);
        u16 destMode;
        u16 srcMode;
        u32 dest;
        s32 offset;

        if (!(control & DMA_ENABLE)
         || !(control & DMA_REPEAT)
         || (control & DMA_START_MASK) != DMA_START_HBLANK
         || (control & DMA_32BIT)
         || ReadU16(dma + 8) != 1)
            continue;

        destMode = control & DMA_DEST_MASK;
        if (destMode != DMA_DEST_FIXED && destMode != DMA_DEST_RELOAD)
            continue;

        dest = ReadU32(dma + 4);
        offset = (s32)(dest - REG_BASE);
        if (offset < 0 || offset >= REG_OFFSET_DMA0 || (offset & 1))
            continue;
        if (sHblankDmaGpuRegs[offset >> 1].active)
            continue;

        srcMode = control & DMA_SRC_MASK;
        sHblankDmaGpuRegs[offset >> 1].active = TRUE;
        sHblankDmaGpuRegs[offset >> 1].src = ReadU32(dma);
        sHblankDmaGpuRegs[offset >> 1].stride = srcMode == DMA_SRC_FIXED ? 0 : srcMode == DMA_SRC_DEC ? -2 : 2;
    }
}

static u16 ScanlineGpuReg(u32 offset, u8 y)
{
    struct HblankDmaGpuReg *dma;

    if (offset < REG_OFFSET_DMA0)
    {
        dma = &sHblankDmaGpuRegs[offset >> 1];
        if (dma->active && y > 0)
        {
            const u32 ptr = dma->src + dma->stride * (y - 1);
            return ReadU16(ptr);
        }
    }

    return ReadU16(REG_BASE + offset);
}

static u8 WindowMask(u8 x, u8 y)
{
    const u16 dispcnt = REG_DISPCNT;
    const u16 windowsEnabled = dispcnt & 0xe000;

    if (!windowsEnabled)
        return WINDOW_ALL_LAYERS;

    if ((dispcnt & 0x2000)
     && InWindowRange(x, ScanlineGpuReg(REG_OFFSET_WIN0H, y))
     && InWindowRange(y, REG_WIN0V))
        return REG_WININ & WINDOW_ALL_LAYERS;

    if ((dispcnt & 0x4000)
     && InWindowRange(x, ScanlineGpuReg(REG_OFFSET_WIN1H, y))
     && InWindowRange(y, REG_WIN1V))
        return (REG_WININ >> 8) & WINDOW_ALL_LAYERS;

    return REG_WINOUT & WINDOW_ALL_LAYERS;
}

static struct Rgb ActiveBlendColor(struct Rgb color, u8 layer, u32 pixel, bool8 effectsEnabled, u8 y, bool8 forceAlphaBlend)
{
    const u16 bldcnt = REG_BLDCNT;
    const u8 effect = (bldcnt >> 6) & 3;
    const u8 sourceTargets = bldcnt & WINDOW_ALL_LAYERS;
    const bool8 isSourceTarget = (sourceTargets & layer) || (forceAlphaBlend && effect == 1);
    u8 evy;

    if ((!effectsEnabled && !(forceAlphaBlend && effect == 1)) || !isSourceTarget || effect == 0)
        return color;

    if (effect == 1 && ((bldcnt >> 8) & sLayerData[pixel]))
    {
        const u16 alpha = REG_BLDALPHA;
        const u8 eva = (alpha & 0x1f) > 16 ? 16 : alpha & 0x1f;
        const u8 evb = ((alpha >> 8) & 0x1f) > 16 ? 16 : (alpha >> 8) & 0x1f;
        struct Rgb blended;
        const u32 p = pixel * RGBA_CHANNELS;

        blended.r = ClampBlend(((u32)color.r * eva + (u32)sWasmDisplayRgba[p] * evb) >> 4);
        blended.g = ClampBlend(((u32)color.g * eva + (u32)sWasmDisplayRgba[p + 1] * evb) >> 4);
        blended.b = ClampBlend(((u32)color.b * eva + (u32)sWasmDisplayRgba[p + 2] * evb) >> 4);
        return blended;
    }

    evy = ScanlineGpuReg(REG_OFFSET_BLDY, y) & 0x1f;
    if (evy > 16)
        evy = 16;

    if (effect == 2)
    {
        color.r = color.r + (((255 - color.r) * evy) >> 4);
        color.g = color.g + (((255 - color.g) * evy) >> 4);
        color.b = color.b + (((255 - color.b) * evy) >> 4);
    }
    else if (effect == 3)
    {
        color.r = color.r - ((color.r * evy) >> 4);
        color.g = color.g - ((color.g * evy) >> 4);
        color.b = color.b - ((color.b * evy) >> 4);
    }

    return color;
}

static void PutPixel(s32 x, s32 y, struct Rgb color, u8 layer, bool8 forceAlphaBlend)
{
    u8 mask;
    u32 pixel;
    u32 p;

    if (x < 0 || y < 0 || x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT)
        return;

    mask = WindowMask(x, y);
    if (layer != LAYER_BACKDROP && !(mask & layer))
        return;

    pixel = y * DISPLAY_WIDTH + x;
    color = ActiveBlendColor(color, layer, pixel, mask & LAYER_BACKDROP, y, forceAlphaBlend);
    p = pixel * RGBA_CHANNELS;
    sWasmDisplayRgba[p] = color.r;
    sWasmDisplayRgba[p + 1] = color.g;
    sWasmDisplayRgba[p + 2] = color.b;
    sWasmDisplayRgba[p + 3] = 255;
    sLayerData[pixel] = layer;
}

static void ClearScreen(void)
{
    const struct Rgb color = GbaColor(ReadU16(BG_PLTT));

    for (u32 y = 0; y < DISPLAY_HEIGHT; y++)
        for (u32 x = 0; x < DISPLAY_WIDTH; x++)
            PutPixel(x, y, color, LAYER_BACKDROP, FALSE);
}

static void RenderBitmapMode3(void)
{
    for (u32 i = 0; i < DISPLAY_PIXELS; i++)
    {
        const struct Rgb color = GbaColor(ReadU16(VRAM + i * 2));
        const u32 p = i * RGBA_CHANNELS;
        sWasmDisplayRgba[p] = color.r;
        sWasmDisplayRgba[p + 1] = color.g;
        sWasmDisplayRgba[p + 2] = color.b;
        sWasmDisplayRgba[p + 3] = 255;
        sLayerData[i] = LAYER_BG2;
    }
}

static void RenderBitmapMode4(u16 dispcnt)
{
    const u32 page = dispcnt & 0x10 ? 0xA000 : 0;

    for (u32 i = 0; i < DISPLAY_PIXELS; i++)
    {
        const u8 colorIndex = *Ptr8(VRAM + page + i);
        const struct Rgb color = GbaColor(ReadU16(PLTT + colorIndex * 2));
        const u32 p = i * RGBA_CHANNELS;
        sWasmDisplayRgba[p] = color.r;
        sWasmDisplayRgba[p + 1] = color.g;
        sWasmDisplayRgba[p + 2] = color.b;
        sWasmDisplayRgba[p + 3] = 255;
        sLayerData[i] = LAYER_BG2;
    }
}

static bool8 TextBgPixel(u8 bg, u8 x, u8 y, struct Rgb *color)
{
    const u16 cnt = ReadU16(REG_BASE + REG_OFFSET_BG0CNT + bg * 2);
    const u32 charBase = VRAM + ((cnt >> 2) & 3) * 0x4000;
    const u32 screenBase = VRAM + ((cnt >> 8) & 31) * 0x800;
    const bool8 color256 = cnt & 0x80;
    const u8 size = (cnt >> 14) & 3;
    const u16 width = size & 1 ? 512 : 256;
    const u16 height = size & 2 ? 512 : 256;
    const u32 hofsOffset = REG_OFFSET_BG0HOFS + bg * 4;
    const u16 hofs = ScanlineGpuReg(hofsOffset, y) & 511;
    const u16 vofs = ScanlineGpuReg(hofsOffset + 2, y) & 511;
    const u16 sx = (x + hofs) & (width - 1);
    const u16 sy = (y + vofs) & (height - 1);
    const u8 block = (sx >= 256 ? 1 : 0) + (sy >= 256 ? (size == 3 ? 2 : 1) : 0);
    const u8 mapX = (sx & 255) >> 3;
    const u8 mapY = (sy & 255) >> 3;
    const u16 entry = ReadU16(screenBase + block * 0x800 + (mapY * 32 + mapX) * 2);
    const u16 tile = entry & 0x3ff;
    const u8 palette = (entry >> 12) & 15;
    const u8 px = entry & 0x400 ? 7 - (sx & 7) : sx & 7;
    const u8 py = entry & 0x800 ? 7 - (sy & 7) : sy & 7;
    u8 colorIndex;

    if (color256)
    {
        colorIndex = *Ptr8(charBase + tile * 64 + py * 8 + px);
        if (!colorIndex)
            return FALSE;
        *color = GbaColor(ReadU16(PLTT + colorIndex * 2));
        return TRUE;
    }

    {
        const u8 packed = *Ptr8(charBase + tile * 32 + py * 4 + (px >> 1));
        colorIndex = px & 1 ? packed >> 4 : packed & 15;
    }
    if (!colorIndex)
        return FALSE;

    *color = GbaColor(ReadU16(PLTT + (palette * 16 + colorIndex) * 2));
    return TRUE;
}

static bool8 AffineBgPixel(u8 bg, u8 x, u8 y, struct Rgb *color)
{
    const u16 cnt = ReadU16(REG_BASE + REG_OFFSET_BG0CNT + bg * 2);
    const u32 charBase = VRAM + ((cnt >> 2) & 3) * 0x4000;
    const u32 screenBase = VRAM + ((cnt >> 8) & 31) * 0x800;
    const u16 sizes[] = {128, 256, 512, 1024};
    const u16 size = sizes[(cnt >> 14) & 3];
    const bool8 wrap = cnt & 0x2000;
    const u8 reg = bg == 2 ? REG_OFFSET_BG2PA : REG_OFFSET_BG3PA;
    const s16 pa = Signed16(ReadU16(REG_BASE + reg));
    const s16 pb = Signed16(ReadU16(REG_BASE + reg + 2));
    const s16 pc = Signed16(ReadU16(REG_BASE + reg + 4));
    const s16 pd = Signed16(ReadU16(REG_BASE + reg + 6));
    const s32 refX = Signed28(Word(reg + 8));
    const s32 refY = Signed28(Word(reg + 12));
    s32 sx = (refX + pa * x + pb * y) >> 8;
    s32 sy = (refY + pc * x + pd * y) >> 8;
    u16 tile;
    u8 colorIndex;

    if (wrap)
    {
        sx &= size - 1;
        sy &= size - 1;
    }
    else if (sx < 0 || sy < 0 || sx >= size || sy >= size)
    {
        return FALSE;
    }

    tile = *Ptr8(screenBase + (sy >> 3) * (size >> 3) + (sx >> 3));
    colorIndex = *Ptr8(charBase + tile * 64 + (sy & 7) * 8 + (sx & 7));
    if (!colorIndex)
        return FALSE;

    *color = GbaColor(ReadU16(PLTT + colorIndex * 2));
    return TRUE;
}

static u8 BgLayersForMode(u16 dispcnt, struct BgLayer *layers)
{
    const u8 mode = dispcnt & 7;
    u8 count = 0;

    for (u8 bg = 0; bg < 4; bg++)
    {
        if (!(dispcnt & (0x100 << bg)))
            continue;

        if (mode == 0)
        {
            layers[count].bg = bg;
            layers[count].type = 0;
            count++;
        }
        else if (mode == 1 && bg < 2)
        {
            layers[count].bg = bg;
            layers[count].type = 0;
            count++;
        }
        else if (mode == 1 && bg == 2)
        {
            layers[count].bg = bg;
            layers[count].type = 1;
            count++;
        }
        else if (mode == 2 && bg >= 2)
        {
            layers[count].bg = bg;
            layers[count].type = 1;
            count++;
        }
    }

    return count;
}

static u16 ObjTileOffset(u16 tileBase, u8 tileX, u8 tileY, u8 width, bool8 color256, bool8 mapping1d)
{
    if (mapping1d)
        return tileBase + tileY * (color256 ? width >> 2 : width >> 3) + tileX * (color256 ? 2 : 1);
    return tileBase + tileY * 32 + tileX * (color256 ? 2 : 1);
}

static bool8 ObjPixel(u16 tileBase, u8 x, u8 y, u8 width, bool8 color256, u8 palette, bool8 mapping1d, struct Rgb *color)
{
    const u16 tileOffset = ObjTileOffset(tileBase, x >> 3, y >> 3, width, color256, mapping1d);
    u8 colorIndex;

    if (color256)
    {
        colorIndex = *Ptr8(VRAM + 0x10000 + tileOffset * 32 + (y & 7) * 8 + (x & 7));
    }
    else
    {
        const u8 packed = *Ptr8(VRAM + 0x10000 + tileOffset * 32 + (y & 7) * 4 + ((x & 7) >> 1));
        colorIndex = x & 1 ? packed >> 4 : packed & 15;
    }

    if (!colorIndex)
        return FALSE;

    *color = GbaColor(ReadU16(OBJ_PLTT + (color256 ? colorIndex : palette * 16 + colorIndex) * 2));
    return TRUE;
}

static void RenderBgLayer(u8 bg, u8 type)
{
    struct Rgb color;
    const u8 layer = 1 << bg;

    for (u32 y = 0; y < DISPLAY_HEIGHT; y++)
    {
        for (u32 x = 0; x < DISPLAY_WIDTH; x++)
        {
            const bool8 hasPixel = type ? AffineBgPixel(bg, x, y, &color) : TextBgPixel(bg, x, y, &color);
            if (hasPixel)
                PutPixel(x, y, color, layer, FALSE);
        }
    }
}

static void RenderSprites(u16 dispcnt, s8 priority)
{
    const bool8 mapping1d = dispcnt & 0x40;
    static const u8 sizes[3][4][2] = {
        {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
        {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
        {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
    };
    struct Rgb color;

    if (!(dispcnt & 0x1000))
        return;

    for (s32 i = 127; i >= 0; i--)
    {
        const u32 base = OAM + i * 8;
        const u16 a0 = ReadU16(base);
        const u16 a1 = ReadU16(base + 2);
        const u16 a2 = ReadU16(base + 4);
        const u8 affineMode = (a0 >> 8) & 3;
        const u8 objMode = (a0 >> 10) & 3;
        const bool8 forceAlphaBlend = objMode == 1;
        const bool8 affine = affineMode & 1;
        const u8 shape = (a0 >> 14) & 3;
        const u8 spritePriority = (a2 >> 10) & 3;
        const bool8 color256 = a0 & 0x2000;
        const u8 palette = (a2 >> 12) & 15;
        const u16 tileBase = a2 & 0x3ff;
        u8 w;
        u8 h;
        s32 ox;
        s32 oy;

        if (!affine && (a0 & 0x0200))
            continue;
        if (shape == 3)
            continue;
        if (priority >= 0 && spritePriority != priority)
            continue;

        w = sizes[shape][(a1 >> 14) & 3][0];
        h = sizes[shape][(a1 >> 14) & 3][1];
        ox = a1 & 511;
        oy = a0 & 255;
        if (ox > 240)
            ox -= 512;
        if (oy > 160)
            oy -= 256;

        if (affine)
        {
            const u8 matrix = (a1 >> 9) & 31;
            const u32 matrixBase = OAM + matrix * 32;
            const s16 pa = Signed16(ReadU16(matrixBase + 6));
            const s16 pb = Signed16(ReadU16(matrixBase + 14));
            const s16 pc = Signed16(ReadU16(matrixBase + 22));
            const s16 pd = Signed16(ReadU16(matrixBase + 30));
            const u16 drawW = affineMode == 3 ? w * 2 : w;
            const u16 drawH = affineMode == 3 ? h * 2 : h;
            const s32 drawCx = drawW / 2;
            const s32 drawCy = drawH / 2;
            const s32 texCx = w / 2;
            const s32 texCy = h / 2;

            for (u32 y = 0; y < drawH; y++)
            {
                for (u32 x = 0; x < drawW; x++)
                {
                    const s32 dx = (s32)x - drawCx;
                    const s32 dy = (s32)y - drawCy;
                    const s32 px = ((pa * dx + pb * dy) >> 8) + texCx;
                    const s32 py = ((pc * dx + pd * dy) >> 8) + texCy;

                    if (px < 0 || py < 0 || px >= w || py >= h)
                        continue;
                    if (ObjPixel(tileBase, px, py, w, color256, palette, mapping1d, &color))
                        PutPixel(ox + x, oy + y, color, LAYER_OBJ, forceAlphaBlend);
                }
            }
        }
        else
        {
            for (u32 y = 0; y < h; y++)
            {
                for (u32 x = 0; x < w; x++)
                {
                    const u8 px = a1 & 0x1000 ? w - 1 - x : x;
                    const u8 py = a1 & 0x2000 ? h - 1 - y : y;

                    if (ObjPixel(tileBase, px, py, w, color256, palette, mapping1d, &color))
                        PutPixel(ox + x, oy + y, color, LAYER_OBJ, forceAlphaBlend);
                }
            }
        }
    }
}

static void RenderTiled(u16 dispcnt)
{
    struct BgLayer layers[4];
    const u8 count = BgLayersForMode(dispcnt, layers);

    ClearScreen();
    for (s8 priority = 3; priority >= 0; priority--)
    {
        for (u8 i = 0; i < count; i++)
        {
            const u8 bg = layers[i].bg;
            if ((ReadU16(REG_BASE + REG_OFFSET_BG0CNT + bg * 2) & 3) == priority)
                RenderBgLayer(bg, layers[i].type);
        }
        RenderSprites(dispcnt, priority);
    }
}

void WasmRefreshHblankDmaGpuRegs(void)
{
    RefreshHblankDmaGpuRegs();
}

u32 WasmWindowMask(u32 x, u32 y)
{
    return WindowMask(x, y);
}

u32 WasmHblankDmaGpuRegActive(u32 offset)
{
    return offset < REG_OFFSET_DMA0 && sHblankDmaGpuRegs[offset >> 1].active;
}

void WasmRenderFrame(void)
{
    const u16 dispcnt = REG_DISPCNT;
    const u8 mode = dispcnt & 7;

    WasmRefreshHblankDmaGpuRegs();
    if (mode == 3)
        RenderBitmapMode3();
    else if (mode == 4)
        RenderBitmapMode4(dispcnt);
    else
        RenderTiled(dispcnt);

    if (mode == 3 || mode == 4)
        RenderSprites(dispcnt, -1);
}

u8 *WasmDisplayBuffer(void)
{
    return sWasmDisplayRgba;
}

u32 WasmDisplayBufferSize(void)
{
    return sizeof(sWasmDisplayRgba);
}

#endif // WASM
