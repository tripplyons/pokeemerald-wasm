#if WASM

#include "global.h"
#include "gba/defines.h"
#include "gba/io_reg.h"

#define DISPLAY_WIDTH 240
#define DISPLAY_HEIGHT 160
#define DISPLAY_PIXELS (DISPLAY_WIDTH * DISPLAY_HEIGHT)

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

// Window registers resolved for one scanline.
struct WindowLine
{
    bool8 win0Active;
    bool8 win1Active;
    u16 win0h;
    u16 win1h;
    u8 win0Mask;
    u8 win1Mask;
    u8 outsideMask;
};

struct BlendState
{
    u8 effect;
    u8 sourceTargets;
    u8 destTargets;
    u8 eva;
    u8 evb;
};

// Pixels are stored as little-endian words holding the bytes R, G, B, A.
static u32 sWasmDisplayRgba[DISPLAY_PIXELS];
static u8 sLayerData[DISPLAY_PIXELS];
static struct HblankDmaGpuReg sHblankDmaGpuRegs[GPU_REG_U16_COUNT];

// Registers and palettes cannot change during a render, so everything that
// depends only on the frame or the scanline is resolved once per frame.
static u32 sPaletteRgba[512];
static u8 sWindowMasks[DISPLAY_PIXELS];
static u8 sBlendEvy[DISPLAY_HEIGHT];
static struct BlendState sBlend;

// A scanline whose pixels all share one window mask stores it here, which
// settles a layer's visibility and blending for the whole line at once.
static u8 sLineMasks[DISPLAY_HEIGHT];
static bool8 sLineMaskUniform[DISPLAY_HEIGHT];

// Brightness effects depend only on the color and the line's BLDY, so they
// are applied to the palette. BLDY_NONE marks the palette as not built.
#define BLDY_NONE 0xff
static u32 sBrightnessPaletteRgba[512];
static u8 sBrightnessPaletteEvy;

// About half of the background tiles on screen are fully transparent. Each
// 32-byte VRAM tile slot is classified at most once per frame. A 256-color
// tile spans two slots, and the last character block can address tiles up
// to 0x1C000.
enum
{
    TILE_SLOT_UNKNOWN,
    TILE_SLOT_EMPTY,
    TILE_SLOT_DRAWN,
};
#define TILE_SLOT_SIZE 32
static u8 sTileSlots[0x1C000 / TILE_SLOT_SIZE];

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

static inline u32 ClampBlend(u32 value)
{
    return value > 255 ? 255 : value;
}

static inline u32 PackRgba(u32 r, u32 g, u32 b)
{
    return r | (g << 8) | (b << 16) | 0xff000000;
}

static inline u32 GbaColor(u16 value)
{
    return PackRgba((value & 31) * 255 / 31,
                    ((value >> 5) & 31) * 255 / 31,
                    ((value >> 10) & 31) * 255 / 31);
}

static inline u32 BrightnessColor(u32 color, u8 effect, u32 evy)
{
    u32 r = color & 0xff;
    u32 g = (color >> 8) & 0xff;
    u32 b = (color >> 16) & 0xff;

    if (effect == 2)
    {
        r = r + (((255 - r) * evy) >> 4);
        g = g + (((255 - g) * evy) >> 4);
        b = b + (((255 - b) * evy) >> 4);
    }
    else
    {
        r = r - ((r * evy) >> 4);
        g = g - ((g * evy) >> 4);
        b = b - ((b * evy) >> 4);
    }

    return PackRgba(r, g, b);
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

static void LoadWindowLine(u8 y, struct WindowLine *line)
{
    const u16 dispcnt = REG_DISPCNT;
    const u16 windowsEnabled = dispcnt & 0xe000;

    line->win0Active = (dispcnt & 0x2000) && InWindowRange(y, REG_WIN0V);
    line->win1Active = (dispcnt & 0x4000) && InWindowRange(y, REG_WIN1V);
    if (line->win0Active)
        line->win0h = ScanlineGpuReg(REG_OFFSET_WIN0H, y);
    if (line->win1Active)
        line->win1h = ScanlineGpuReg(REG_OFFSET_WIN1H, y);
    line->win0Mask = REG_WININ & WINDOW_ALL_LAYERS;
    line->win1Mask = (REG_WININ >> 8) & WINDOW_ALL_LAYERS;
    line->outsideMask = windowsEnabled ? REG_WINOUT & WINDOW_ALL_LAYERS : WINDOW_ALL_LAYERS;
}

static inline u8 WindowMaskAt(u8 x, const struct WindowLine *line)
{
    if (line->win0Active && InWindowRange(x, line->win0h))
        return line->win0Mask;
    if (line->win1Active && InWindowRange(x, line->win1h))
        return line->win1Mask;
    return line->outsideMask;
}

static u8 WindowMask(u8 x, u8 y)
{
    struct WindowLine line;

    LoadWindowLine(y, &line);
    return WindowMaskAt(x, &line);
}

static inline u32 ActiveBlendColor(u32 color, u8 layer, u32 pixel, bool8 effectsEnabled, u8 y, bool8 forceAlphaBlend)
{
    const u8 effect = sBlend.effect;
    const bool8 forcedAlpha = forceAlphaBlend && effect == 1;
    const bool8 isSourceTarget = (sBlend.sourceTargets & layer) || forcedAlpha;
    u32 below;

    if ((!effectsEnabled && !forcedAlpha) || !isSourceTarget || effect == 0)
        return color;

    if (effect != 1)
        return BrightnessColor(color, effect, sBlendEvy[y]);

    if (!(sBlend.destTargets & sLayerData[pixel]))
        return color;

    below = sWasmDisplayRgba[pixel];
    return PackRgba(ClampBlend(((color & 0xff) * sBlend.eva + (below & 0xff) * sBlend.evb) >> 4),
                    ClampBlend((((color >> 8) & 0xff) * sBlend.eva + ((below >> 8) & 0xff) * sBlend.evb) >> 4),
                    ClampBlend((((color >> 16) & 0xff) * sBlend.eva + ((below >> 16) & 0xff) * sBlend.evb) >> 4));
}

// Callers guarantee x < DISPLAY_WIDTH and y < DISPLAY_HEIGHT.
static inline void PutPixel(u32 x, u32 y, u32 color, u8 layer, bool8 forceAlphaBlend)
{
    const u32 pixel = y * DISPLAY_WIDTH + x;
    const u8 mask = sWindowMasks[pixel];

    if (layer != LAYER_BACKDROP && !(mask & layer))
        return;

    sWasmDisplayRgba[pixel] = ActiveBlendColor(color, layer, pixel, mask & LAYER_BACKDROP, y, forceAlphaBlend);
    sLayerData[pixel] = layer;
}

// The browser build links no libc, so fills are written as loops.
static inline void FillBytes(u8 *dest, u8 value, u32 count)
{
    for (u32 i = 0; i < count; i++)
        dest[i] = value;
}

// Covers the same pixels as InWindowRange, including ranges that wrap.
static void FillWindowRange(u8 *masks, u16 range, u8 mask)
{
    const u32 rawStart = range >> 8;
    const u32 rawEnd = range & 0xff;
    const u32 start = rawStart > DISPLAY_WIDTH ? DISPLAY_WIDTH : rawStart;
    const u32 end = rawEnd > DISPLAY_WIDTH ? DISPLAY_WIDTH : rawEnd;

    if (rawStart <= rawEnd)
    {
        FillBytes(masks + start, mask, end - start);
    }
    else
    {
        FillBytes(masks + start, mask, DISPLAY_WIDTH - start);
        FillBytes(masks, mask, end);
    }
}

static void PrepareFrame(void)
{
    const u16 *pltt = (const u16 *)PLTT;
    const u16 bldcnt = REG_BLDCNT;
    const u16 alpha = REG_BLDALPHA;

    for (u32 i = 0; i < ARRAY_COUNT(sPaletteRgba); i++)
        sPaletteRgba[i] = GbaColor(pltt[i]);

    sBlend.effect = (bldcnt >> 6) & 3;
    sBlend.sourceTargets = bldcnt & WINDOW_ALL_LAYERS;
    sBlend.destTargets = (bldcnt >> 8) & WINDOW_ALL_LAYERS;
    sBlend.eva = (alpha & 0x1f) > 16 ? 16 : alpha & 0x1f;
    sBlend.evb = ((alpha >> 8) & 0x1f) > 16 ? 16 : (alpha >> 8) & 0x1f;

    sBrightnessPaletteEvy = BLDY_NONE;
    FillBytes(sTileSlots, TILE_SLOT_UNKNOWN, sizeof(sTileSlots));

    for (u32 y = 0; y < DISPLAY_HEIGHT; y++)
    {
        struct WindowLine line;
        u8 *masks = &sWindowMasks[y * DISPLAY_WIDTH];
        u8 differing = 0;

        LoadWindowLine(y, &line);
        FillBytes(masks, line.outsideMask, DISPLAY_WIDTH);
        if (line.win1Active)
            FillWindowRange(masks, line.win1h, line.win1Mask);
        if (line.win0Active)
            FillWindowRange(masks, line.win0h, line.win0Mask);

        for (u32 x = 1; x < DISPLAY_WIDTH; x++)
            differing |= masks[x] ^ masks[0];
        sLineMasks[y] = masks[0];
        sLineMaskUniform[y] = !differing;

        if (sBlend.effect >= 2)
        {
            const u8 evy = ScanlineGpuReg(REG_OFFSET_BLDY, y) & 0x1f;
            sBlendEvy[y] = evy > 16 ? 16 : evy;
        }
    }
}

static const u32 *BrightnessPalette(u8 evy)
{
    if (sBrightnessPaletteEvy != evy)
    {
        for (u32 i = 0; i < ARRAY_COUNT(sPaletteRgba); i++)
            sBrightnessPaletteRgba[i] = BrightnessColor(sPaletteRgba[i], sBlend.effect, evy);
        sBrightnessPaletteEvy = evy;
    }

    return sBrightnessPaletteRgba;
}

enum
{
    LINE_HIDDEN,  // The window hides the layer on the whole line.
    LINE_DIRECT,  // Every pixel is a plain store from the returned palette.
    LINE_GENERAL, // Pixels need PutPixel's per-pixel window and blend checks.
};

// Decides once how a layer reaches a scanline. Alpha blending reads the
// pixel below, so it always takes the general path.
static u8 ResolveLine(u8 layer, u32 y, const u32 **palette)
{
    const u8 mask = sLineMasks[y];

    if (!sLineMaskUniform[y])
        return LINE_GENERAL;
    if (layer != LAYER_BACKDROP && !(mask & layer))
        return LINE_HIDDEN;

    *palette = sPaletteRgba;
    if (sBlend.effect == 0 || !(sBlend.sourceTargets & layer) || !(mask & LAYER_BACKDROP))
        return LINE_DIRECT;
    if (sBlend.effect == 1)
        return LINE_GENERAL;

    *palette = BrightnessPalette(sBlendEvy[y]);
    return LINE_DIRECT;
}

static void ClearScreen(void)
{
    for (u32 y = 0; y < DISPLAY_HEIGHT; y++)
    {
        const u32 *palette;

        if (ResolveLine(LAYER_BACKDROP, y, &palette) == LINE_DIRECT)
        {
            u32 *dest = &sWasmDisplayRgba[y * DISPLAY_WIDTH];

            for (u32 x = 0; x < DISPLAY_WIDTH; x++)
                dest[x] = palette[0];
            FillBytes(&sLayerData[y * DISPLAY_WIDTH], LAYER_BACKDROP, DISPLAY_WIDTH);
        }
        else
        {
            for (u32 x = 0; x < DISPLAY_WIDTH; x++)
                PutPixel(x, y, sPaletteRgba[0], LAYER_BACKDROP, FALSE);
        }
    }
}

static inline u32 ReverseNibbles(u32 value)
{
    value = ((value >> 4) & 0x0f0f0f0f) | ((value & 0x0f0f0f0f) << 4);
    value = ((value >> 8) & 0x00ff00ff) | ((value & 0x00ff00ff) << 8);
    return (value >> 16) | (value << 16);
}

static inline u64 ReverseBytes(u64 value)
{
    value = ((value >> 8) & 0x00ff00ff00ff00ffULL) | ((value & 0x00ff00ff00ff00ffULL) << 8);
    value = ((value >> 16) & 0x0000ffff0000ffffULL) | ((value & 0x0000ffff0000ffffULL) << 16);
    return (value >> 32) | (value << 32);
}

// The tile-row plotters take color indices packed lowest-first in screen
// order, with zero for every pixel outside the run. A row with no
// transparent pixel is always a whole tile.
static inline void PlotTileRow4(u32 *dest, u8 *layers, u32 packed, const u32 *palette, u8 layer)
{
    if (!((packed - 0x11111111) & ~packed & 0x88888888))
    {
        for (u32 i = 0; i < 8; i++)
        {
            dest[i] = palette[(packed >> (i * 4)) & 15];
            layers[i] = layer;
        }
        return;
    }

    for (; packed; packed >>= 4, dest++, layers++)
    {
        if (packed & 15)
        {
            *dest = palette[packed & 15];
            *layers = layer;
        }
    }
}

static inline void PlotTileRow8(u32 *dest, u8 *layers, u64 packed, const u32 *palette, u8 layer)
{
    if (!((packed - 0x0101010101010101ULL) & ~packed & 0x8080808080808080ULL))
    {
        for (u32 i = 0; i < 8; i++)
        {
            dest[i] = palette[(packed >> (i * 8)) & 255];
            layers[i] = layer;
        }
        return;
    }

    for (; packed; packed >>= 8, dest++, layers++)
    {
        if (packed & 255)
        {
            *dest = palette[packed & 255];
            *layers = layer;
        }
    }
}

static void RenderBitmapMode3(void)
{
    const u16 *vram = (const u16 *)VRAM;

    for (u32 i = 0; i < DISPLAY_PIXELS; i++)
    {
        sWasmDisplayRgba[i] = GbaColor(vram[i]);
        sLayerData[i] = LAYER_BG2;
    }
}

static void RenderBitmapMode4(u16 dispcnt)
{
    const u8 *vram = (const u8 *)VRAM + (dispcnt & 0x10 ? 0xA000 : 0);

    for (u32 i = 0; i < DISPLAY_PIXELS; i++)
    {
        sWasmDisplayRgba[i] = sPaletteRgba[vram[i]];
        sLayerData[i] = LAYER_BG2;
    }
}

static bool8 TileSlotEmpty(u32 slot)
{
    if (sTileSlots[slot] == TILE_SLOT_UNKNOWN)
    {
        const u32 *words = (const u32 *)(VRAM + slot * TILE_SLOT_SIZE);
        u32 drawn = 0;

        for (u32 i = 0; i < TILE_SLOT_SIZE / 4; i++)
            drawn |= words[i];
        sTileSlots[slot] = drawn ? TILE_SLOT_DRAWN : TILE_SLOT_EMPTY;
    }

    return sTileSlots[slot] == TILE_SLOT_EMPTY;
}

// Returns one bit per map column whose tile draws anything. A 32-column map
// repeats its bits in the upper half, so rotating the result by the first
// visible column wraps the same way the map does.
static u64 DrawnTileColumns(const u16 *mapRow, u32 columns, u32 charSlot, bool8 color256)
{
    u64 drawn = 0;

    for (u32 column = 0; column < columns; column++)
    {
        const u16 tile = mapRow[(column >> 5) * 0x400 + (column & 31)] & 0x3ff;
        const u32 slot = charSlot + (color256 ? tile * 2 : tile);

        if (!TileSlotEmpty(slot) || (color256 && !TileSlotEmpty(slot + 1)))
            drawn |= 1ULL << column;
    }

    return columns == 32 ? drawn | (drawn << 32) : drawn;
}

static void RenderTextBg(u8 bg)
{
    const u8 *vram = (const u8 *)VRAM;
    const u16 cnt = ReadU16(REG_BASE + REG_OFFSET_BG0CNT + bg * 2);
    const u8 *chars = vram + ((cnt >> 2) & 3) * 0x4000;
    const u8 *screen = vram + ((cnt >> 8) & 31) * 0x800;
    const bool8 color256 = (cnt & 0x80) != 0;
    const u8 size = (cnt >> 14) & 3;
    const u16 width = size & 1 ? 512 : 256;
    const u16 height = size & 2 ? 512 : 256;
    const u32 hofsOffset = REG_OFFSET_BG0HOFS + bg * 4;
    const u8 layer = 1 << bg;
    const u16 *drawnRow = NULL;
    u64 drawnColumns = 0;

    for (u32 y = 0; y < DISPLAY_HEIGHT; y++)
    {
        const u16 hofs = ScanlineGpuReg(hofsOffset, y) & 511;
        const u16 vofs = ScanlineGpuReg(hofsOffset + 2, y) & 511;
        const u16 sy = (y + vofs) & (height - 1);
        const u8 rowBlock = sy >= 256 ? (size == 3 ? 2 : 1) : 0;
        const u16 *mapRow = (const u16 *)(screen + rowBlock * 0x800 + ((sy & 255) >> 3) * 64);
        const u32 *linePalette;
        const u8 lineMode = ResolveLine(layer, y, &linePalette);
        const u32 slots = (DISPLAY_WIDTH + (hofs & 7) + 7) >> 3;
        u64 visible;

        if (lineMode == LINE_HIDDEN)
            continue;

        // The eight scanlines that cross a map row share its drawn columns.
        if (mapRow != drawnRow)
        {
            drawnColumns = DrawnTileColumns(mapRow, width >> 3, (chars - vram) / TILE_SLOT_SIZE, color256);
            drawnRow = mapRow;
        }

        // Bit k is the k-th tile on the line, which starts at x = k * 8 - (hofs & 7).
        visible = (drawnColumns >> (hofs >> 3)) | (drawnColumns << ((64 - (hofs >> 3)) & 63));
        visible &= (1ULL << slots) - 1;

        // Each run stays inside one tile, so its map entry is read once.
        for (; visible; visible &= visible - 1)
        {
            const s32 tileX = __builtin_ctzll(visible) * 8 - (hofs & 7);
            const u32 x = tileX < 0 ? 0 : tileX;
            const u16 sx = (x + hofs) & (width - 1);
            const u16 entry = mapRow[(sx >> 8) * 0x400 + ((sx & 255) >> 3)];
            const u16 tile = entry & 0x3ff;
            const bool8 flipX = (entry & 0x400) != 0;
            const u8 py = entry & 0x800 ? 7 - (sy & 7) : sy & 7;
            u32 run = 8 - (sx & 7);

            if (run > DISPLAY_WIDTH - x)
                run = DISPLAY_WIDTH - x;

            if (lineMode == LINE_DIRECT)
            {
                u32 *dest = &sWasmDisplayRgba[y * DISPLAY_WIDTH + x];
                u8 *layers = &sLayerData[y * DISPLAY_WIDTH + x];

                if (color256)
                {
                    const u8 *tileRow = chars + tile * 64 + py * 8;
                    u64 packed = 0;

                    for (u32 i = 0; i < 8; i++)
                        packed |= (u64)tileRow[i] << (i * 8);
                    if (flipX)
                        packed = ReverseBytes(packed);
                    packed >>= (sx & 7) * 8;
                    if (run < 8)
                        packed &= (1ULL << (run * 8)) - 1;
                    PlotTileRow8(dest, layers, packed, linePalette, layer);
                }
                else
                {
                    const u8 *tileRow = chars + tile * 32 + py * 4;
                    u32 packed = tileRow[0] | (tileRow[1] << 8) | (tileRow[2] << 16) | ((u32)tileRow[3] << 24);

                    if (flipX)
                        packed = ReverseNibbles(packed);
                    packed >>= (sx & 7) * 4;
                    if (run < 8)
                        packed &= (1u << (run * 4)) - 1;
                    PlotTileRow4(dest, layers, packed, &linePalette[((entry >> 12) & 15) * 16], layer);
                }
            }
            else if (color256)
            {
                const u8 *tileRow = chars + tile * 64 + py * 8;

                for (u32 i = 0; i < run; i++)
                {
                    const u8 px = flipX ? 7 - ((sx + i) & 7) : (sx + i) & 7;
                    const u8 colorIndex = tileRow[px];

                    if (colorIndex)
                        PutPixel(x + i, y, sPaletteRgba[colorIndex], layer, FALSE);
                }
            }
            else
            {
                const u8 *tileRow = chars + tile * 32 + py * 4;
                const u32 packed = tileRow[0] | (tileRow[1] << 8) | (tileRow[2] << 16) | ((u32)tileRow[3] << 24);
                const u32 *palette = &sPaletteRgba[((entry >> 12) & 15) * 16];

                for (u32 i = 0; packed && i < run; i++)
                {
                    const u8 px = flipX ? 7 - ((sx + i) & 7) : (sx + i) & 7;
                    const u8 colorIndex = (packed >> (px * 4)) & 15;

                    if (colorIndex)
                        PutPixel(x + i, y, palette[colorIndex], layer, FALSE);
                }
            }
        }
    }
}

static void RenderAffineBg(u8 bg)
{
    const u8 *vram = (const u8 *)VRAM;
    const u16 cnt = ReadU16(REG_BASE + REG_OFFSET_BG0CNT + bg * 2);
    const u8 *chars = vram + ((cnt >> 2) & 3) * 0x4000;
    const u8 *screen = vram + ((cnt >> 8) & 31) * 0x800;
    const u16 sizes[] = {128, 256, 512, 1024};
    const u16 size = sizes[(cnt >> 14) & 3];
    const bool8 wrap = (cnt & 0x2000) != 0;
    const u8 reg = bg == 2 ? REG_OFFSET_BG2PA : REG_OFFSET_BG3PA;
    const s16 pa = Signed16(ReadU16(REG_BASE + reg));
    const s16 pb = Signed16(ReadU16(REG_BASE + reg + 2));
    const s16 pc = Signed16(ReadU16(REG_BASE + reg + 4));
    const s16 pd = Signed16(ReadU16(REG_BASE + reg + 6));
    const s32 refX = Signed28(Word(reg + 8));
    const s32 refY = Signed28(Word(reg + 12));
    const u8 layer = 1 << bg;

    for (s32 y = 0; y < DISPLAY_HEIGHT; y++)
    {
        for (s32 x = 0; x < DISPLAY_WIDTH; x++)
        {
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
                continue;
            }

            tile = screen[(sy >> 3) * (size >> 3) + (sx >> 3)];
            colorIndex = chars[tile * 64 + (sy & 7) * 8 + (sx & 7)];
            if (colorIndex)
                PutPixel(x, y, sPaletteRgba[colorIndex], layer, FALSE);
        }
    }
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

static inline bool8 ObjPixel(u16 tileBase, u8 x, u8 y, u8 width, bool8 color256, u8 palette, bool8 mapping1d, u32 *color)
{
    const u8 *tiles = (const u8 *)(VRAM + 0x10000);
    const u32 *objPalette = &sPaletteRgba[256];
    const u16 tileOffset = ObjTileOffset(tileBase, x >> 3, y >> 3, width, color256, mapping1d);
    u8 colorIndex;

    if (color256)
    {
        colorIndex = tiles[tileOffset * 32 + (y & 7) * 8 + (x & 7)];
    }
    else
    {
        const u8 packed = tiles[tileOffset * 32 + (y & 7) * 4 + ((x & 7) >> 1)];
        colorIndex = x & 1 ? packed >> 4 : packed & 15;
    }

    if (!colorIndex)
        return FALSE;

    *color = objPalette[color256 ? colorIndex : palette * 16 + colorIndex];
    return TRUE;
}

static void RenderBgLayer(u8 bg, u8 type)
{
    if (type)
        RenderAffineBg(bg);
    else
        RenderTextBg(bg);
}

static void RenderSprites(u16 dispcnt, s8 priority)
{
    const bool8 mapping1d = dispcnt & 0x40;
    static const u8 sizes[3][4][2] = {
        {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
        {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
        {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
    };
    const u16 *oam = (const u16 *)OAM;
    u32 color;

    if (!(dispcnt & 0x1000))
        return;

    for (s32 i = 127; i >= 0; i--)
    {
        const u16 a0 = oam[i * 4];
        const u16 a1 = oam[i * 4 + 1];
        const u16 a2 = oam[i * 4 + 2];
        const u8 affineMode = (a0 >> 8) & 3;
        const u8 objMode = (a0 >> 10) & 3;
        const bool8 forceAlphaBlend = objMode == 1;
        const bool8 affine = affineMode & 1;
        const u8 shape = (a0 >> 14) & 3;
        const u8 spritePriority = (a2 >> 10) & 3;
        const bool8 color256 = (a0 & 0x2000) != 0;
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
            const u16 *matrix = &oam[((a1 >> 9) & 31) * 16];
            const s16 pa = Signed16(matrix[3]);
            const s16 pb = Signed16(matrix[7]);
            const s16 pc = Signed16(matrix[11]);
            const s16 pd = Signed16(matrix[15]);
            const u16 drawW = affineMode == 3 ? w * 2 : w;
            const u16 drawH = affineMode == 3 ? h * 2 : h;
            const s32 drawCx = drawW / 2;
            const s32 drawCy = drawH / 2;
            const s32 texCx = w / 2;
            const s32 texCy = h / 2;

            for (u32 y = 0; y < drawH; y++)
            {
                if ((u32)(oy + y) >= DISPLAY_HEIGHT)
                    continue;

                for (u32 x = 0; x < drawW; x++)
                {
                    const s32 dx = (s32)x - drawCx;
                    const s32 dy = (s32)y - drawCy;
                    const s32 px = ((pa * dx + pb * dy) >> 8) + texCx;
                    const s32 py = ((pc * dx + pd * dy) >> 8) + texCy;

                    if ((u32)(ox + x) >= DISPLAY_WIDTH)
                        continue;
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
                if ((u32)(oy + y) >= DISPLAY_HEIGHT)
                    continue;

                for (u32 x = 0; x < w; x++)
                {
                    const u8 px = a1 & 0x1000 ? w - 1 - x : x;
                    const u8 py = a1 & 0x2000 ? h - 1 - y : y;

                    if ((u32)(ox + x) >= DISPLAY_WIDTH)
                        continue;
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
    PrepareFrame();
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
    return (u8 *)sWasmDisplayRgba;
}

u32 WasmDisplayBufferSize(void)
{
    return sizeof(sWasmDisplayRgba);
}

#endif // WASM
