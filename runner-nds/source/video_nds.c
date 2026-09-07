/* video_nds.c — NES PPU mapped onto NDS 2D hardware.
 *
 * BG:      NES nametable -> NDS text BG map (512x256 covers both nametables).
 * Tiles:   NES 2bpp planar CHR -> NDS 4bpp linear, converted once at load.
 * Sprites: NES OAM -> NDS OAM, 1:1 (NES has 64, NDS has 128).
 *
 * Vertical: NES is 240 lines, DS is 192. We show NES lines 32..223, i.e. the
 * playfield below the status bar. The HUD needs the mid-frame scroll split
 * before it can go on the sub screen; not done yet.
 */
#include <nds.h>
#include <string.h>
#include "nes_runtime.h"

#define NES_TOP_LINE 32          /* first NES scanline shown on the top screen */

static int      s_bg;
static u16     *s_map;
static int      s_bg_hud;        /* sub-screen BG holding the NES status bar */
static u16     *s_map_hud;
static int      s_inited;

/* Standard NES system palette, RGB888. */
static const u8 s_nes_rgb[64][3] = {
    { 84, 84, 84},{  0, 30,116},{  8, 16,144},{ 48,  0,136},{ 68,  0,100},{ 92,  0, 48},{ 84,  4,  0},{ 60, 24,  0},
    { 32, 42,  0},{  8, 58,  0},{  0, 64,  0},{  0, 60,  0},{  0, 50, 60},{  0,  0,  0},{  0,  0,  0},{  0,  0,  0},
    {152,150,152},{  8, 76,196},{ 48, 50,236},{ 92, 30,228},{136, 20,176},{160, 20,100},{152, 34, 32},{120, 60,  0},
    { 84, 90,  0},{ 40,114,  0},{  8,124,  0},{  0,118, 40},{  0,102,120},{  0,  0,  0},{  0,  0,  0},{  0,  0,  0},
    {236,238,236},{ 76,154,236},{120,124,236},{176, 98,236},{228, 84,236},{236, 88,180},{236,106,100},{212,136, 32},
    {160,170,  0},{116,196,  0},{ 76,208, 32},{ 56,204,108},{ 56,180,204},{ 60, 60, 60},{  0,  0,  0},{  0,  0,  0},
    {236,238,236},{168,204,236},{188,188,236},{212,178,236},{236,174,236},{236,174,212},{236,180,176},{228,196,144},
    {204,210,120},{180,222,120},{168,226,144},{152,226,180},{160,214,228},{160,162,160},{  0,  0,  0},{  0,  0,  0},
};

static inline u16 nes_color(u8 idx) {
    const u8 *c = s_nes_rgb[idx & 0x3F];
    return ARGB16(1, c[0] >> 3, c[1] >> 3, c[2] >> 3);
}

/* NES CHR is 2bpp planar: 8 bytes of plane 0, then 8 bytes of plane 1.
 * NDS 4bpp is linear: one byte per two pixels, low nibble = left pixel. */
static void convert_chr(u16 *dst_bg, u16 *dst_spr) {
    for (int t = 0; t < 512; t++) {
        const u8 *src = &g_chr_ram[t * 16];
        u8 tile[32];
        for (int y = 0; y < 8; y++) {
            u8 p0 = src[y], p1 = src[y + 8];
            for (int x = 0; x < 8; x += 2) {
                u8 lo = (((p0 >> (7 - x)) & 1) | (((p1 >> (7 - x)) & 1) << 1));
                u8 hi = (((p0 >> (6 - x)) & 1) | (((p1 >> (6 - x)) & 1) << 1));
                tile[y * 4 + x / 2] = (u8)(lo | (hi << 4));
            }
        }
        memcpy((u8 *)dst_bg  + t * 32, tile, 32);
        memcpy((u8 *)dst_spr + t * 32, tile, 32);
    }
}

static void convert_chr_bg(u16 *dst) {
    for (int t = 0; t < 512; t++) {
        const u8 *src = &g_chr_ram[t * 16];
        u8 tile[32];
        for (int y = 0; y < 8; y++) {
            u8 p0 = src[y], p1 = src[y + 8];
            for (int x = 0; x < 8; x += 2) {
                u8 lo = (((p0 >> (7 - x)) & 1) | (((p1 >> (7 - x)) & 1) << 1));
                u8 hi = (((p0 >> (6 - x)) & 1) | (((p1 >> (6 - x)) & 1) << 1));
                tile[y * 4 + x / 2] = (u8)(lo | (hi << 4));
            }
        }
        memcpy((u8 *)dst + t * 32, tile, 32);
    }
}

void video_init(void) {
    videoSetMode(MODE_0_2D | DISPLAY_BG0_ACTIVE | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankB(VRAM_B_MAIN_SPRITE);

    s_bg  = bgInit(0, BgType_Text4bpp, BgSize_T_512x256, 0, 2);
    s_map = (u16 *)bgGetMapPtr(s_bg);

    oamInit(&oamMain, SpriteMapping_1D_32, false);
    convert_chr((u16 *)bgGetGfxPtr(s_bg), (u16 *)SPRITE_GFX);

    /* Status bar on the sub screen. consoleDemoInit owns sub BG0 with map
     * base 31 and tile base 0. mapBase is 5 bits (0-31) and tileBase 4 bits
     * (0-15), so: map base 30 = 60-62KB, tile base 2 = 32-48KB. Neither
     * collides with the console or with each other. */
    s_bg_hud  = bgInitSub(1, BgType_Text4bpp, BgSize_T_256x256, 30, 2);
    s_map_hud = (u16 *)bgGetMapPtr(s_bg_hud);
    convert_chr_bg((u16 *)bgGetGfxPtr(s_bg_hud));
    bgSetScroll(s_bg_hud, 0, 0);
    bgUpdate();

    /* Drop the console below the 4 HUD rows so both are readable. */
    consoleSetWindow(NULL, 0, 4, 32, 20);

    s_inited = 1;
}

/* NES attribute byte packs four 16x16 quadrants; pick this tile's 2 bits. */
static inline u8 attr_for(const u8 *nt, int col, int row) {
    u8 a = nt[0x3C0 + (row >> 2) * 8 + (col >> 2)];
    int shift = ((row & 2) << 1) | (col & 2);
    return (a >> shift) & 3;
}

static void push_nametable(const u8 *nt, u16 *map, int bg_tile_base) {
    for (int row = 0; row < 30; row++) {
        for (int col = 0; col < 32; col++) {
            u8 t = nt[row * 32 + col];
            map[row * 32 + col] = (u16)(bg_tile_base + t) | (u16)(attr_for(nt, col, row) << 12);
        }
    }
    for (int i = 30 * 32; i < 32 * 32; i++) map[i] = 0;
}

/* Shadow buffers built during the frame, DMA'd to VRAM during vblank.
 * Building straight into VRAM overruns the ~1.3ms vblank window (1920 map
 * entries with an attribute lookup each), so the tail of the writes lands
 * mid-display and tears. */
static u16 s_shadow[2048];      /* 512x256 text BG = two 32x32 screenblocks */
static u16 s_shadow_hud[128];   /* 4 rows x 32 cols */
static u16 s_pal_bg[64], s_pal_spr[64], s_pal_sub[128];
static int s_scroll_x;

static void build_nametable(const u8 *nt, u16 *dst, int bg_tile_base) {
    for (int row = 0; row < 30; row++)
        for (int col = 0; col < 32; col++) {
            u8 t = nt[row * 32 + col];
            dst[row * 32 + col] =
                (u16)(bg_tile_base + t) | (u16)(attr_for(nt, col, row) << 12);
        }
    for (int i = 30 * 32; i < 32 * 32; i++) dst[i] = 0;
}

/* Phase 1: runs right after the game's NMI handler, outside vblank. */
void video_build(void) {
    if (!s_inited) return;

    s_pal_bg[0] = nes_color(g_ppu_pal[0]);
    for (int p = 0; p < 4; p++)
        for (int c = 1; c < 4; c++)
            s_pal_bg[p * 16 + c] = nes_color(g_ppu_pal[p * 4 + c]);

    for (int p = 0; p < 4; p++) {
        s_pal_spr[p * 16] = 0;
        for (int c = 1; c < 4; c++)
            s_pal_spr[p * 16 + c] = nes_color(g_ppu_pal[0x10 + p * 4 + c]);
    }

    for (int p = 0; p < 4; p++)
        for (int c = 1; c < 4; c++)
            s_pal_sub[(p + 4) * 16 + c] = nes_color(g_ppu_pal[p * 4 + c]);

    int bg_base  = (g_ppuctrl & 0x10) ? 256 : 0;
    int spr_base = (g_ppuctrl & 0x08) ? 256 : 0;

    build_nametable(&g_ppu_nt[0x000], s_shadow,        bg_base);
    build_nametable(&g_ppu_nt[0x400], s_shadow + 1024, bg_base);

    /* Status bar: NES lines 0..31 = nametable 0 rows 0..3, never scrolled. */
    {
        const u8 *nt = &g_ppu_nt[0x000];
        for (int row = 0; row < 4; row++)
            for (int col = 0; col < 32; col++) {
                u8 t = nt[row * 32 + col];
                s_shadow_hud[row * 32 + col] =
                    (u16)(bg_base + t) | (u16)((attr_for(nt, col, row) + 4) << 12);
            }
    }

    s_scroll_x = g_ppuscroll_x + ((g_ppuctrl & 1) ? 256 : 0);

    /* oamSet writes into libnds' RAM copy; oamUpdate DMAs it in vblank. */
    for (int i = 0; i < 64; i++) {
        const u8 *o = &g_ppu_oam[i * 4];
        int y = o[0] + 1 - NES_TOP_LINE;
        int x = o[3];
        u8  tile = o[1], attr = o[2];
        int hidden = (o[0] >= 0xEF) || (y < -8) || (y > 192);
        oamSet(&oamMain, i, x, y,
               (attr & 0x20) ? 2 : 0, attr & 3,
               SpriteSize_8x8, SpriteColorFormat_16Color,
               (u8 *)SPRITE_GFX + (spr_base + tile) * 32,
               -1, false, hidden,
               (attr & 0x40) != 0, (attr & 0x80) != 0, false);
    }
}

/* Phase 2: runs inside vblank. DMA only — no computation. */
void video_flush(void) {
    if (!s_inited) return;
    dmaCopy(s_shadow,     s_map,          sizeof s_shadow);
    dmaCopy(s_shadow_hud, s_map_hud,      sizeof s_shadow_hud);
    dmaCopy(s_pal_bg,     BG_PALETTE,     sizeof s_pal_bg);
    dmaCopy(s_pal_spr,    SPRITE_PALETTE, sizeof s_pal_spr);
    dmaCopy(s_pal_sub,    BG_PALETTE_SUB, sizeof s_pal_sub);
    bgSetScroll(s_bg, s_scroll_x, NES_TOP_LINE);
    bgUpdate();
    oamUpdate(&oamMain);
}
