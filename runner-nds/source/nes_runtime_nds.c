/* NDS runtime: ROM loading, NES memory map, vblank/NMI pump.
 * No video yet — PPU writes are recorded into shadow state only. */
#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <string.h>
#include "nes_runtime.h"

extern void func_NMI(void);
extern void video_init(void);
void nes_timing_init(void);
extern void video_build(void);
extern void video_flush(void);
extern void apu_init(void);
extern void apu_frame(void);
extern void apu_write(uint16_t addr, uint8_t val);
extern uint8_t apu_read_status(void);

CPU6502State g_cpu;
uint8_t  g_ram[0x0800];
uint8_t  g_sram[0x2000];
uint8_t  g_chr_ram[0x2000];
uint8_t  g_ppu_oam[0x100];
uint8_t  g_ppu_pal[0x20];
uint8_t  g_ppu_nt[0x1000];
uint8_t  g_ppuctrl, g_ppumask, g_ppustatus;
uint8_t  g_ppuscroll_x, g_ppuscroll_y;
uint8_t  g_oamaddr;
uint8_t  g_controller1_buttons, g_controller2_buttons;
int      g_chr_is_rom = 1;
int      g_bail_active = 0;
uint16_t g_code_window_base = 0x8000;

static uint8_t  s_prg[0x8000];          /* NROM-256: 32K at $8000 */
static uint16_t s_ppuaddr;
static int      s_wtoggle;
static uint8_t  s_ctrl_shift[2];
static int      s_ctrl_strobe;

uint64_t g_frame_count = 0;

/* Debug overlay: L toggles it. Off by default — console writes are slow on
 * hardware, and printing every frame was costing real time. Timer 0/1 are
 * cascaded into one free-running 32-bit counter at the full bus clock, so a
 * tick is ~30ns and it wraps about every two minutes (we only ever diff over
 * a single second). */
static int      s_debug_on;
static uint32_t s_work_acc;      /* summed work ticks over 60 frames  */
static uint32_t s_total_acc;     /* summed frame PERIODS, start to start */
static uint16_t s_last_start;    /* previous frame's start tick */
static int      s_have_last;
static uint32_t s_fps, s_work_us;

/* One 16-bit timer at DIV_1024: 33513982/1024 = 32729 Hz, so a tick is
 * ~30.5us and it wraps every 2 seconds. Unsigned 16-bit subtraction wraps
 * correctly, so per-frame diffs are always right. A cascaded 32-bit pair
 * would give finer resolution but the two halves cannot be read atomically,
 * which is what produced the nonsense numbers. */
static inline uint16_t tmr(void) { return TIMER0_DATA; }

static void timing_init(void) {
    TIMER0_DATA = 0;
    TIMER0_CR   = TIMER_DIV_1024 | TIMER_ENABLE;
}

static inline uint32_t ticks_to_us(uint32_t ticks) {
    return (ticks * 3052u) / 100u;      /* 1 tick = 30.52us */
}

/* NTSC NES runs at 60.0988 Hz; the DS panel is fixed at 59.8261 Hz, so one
 * NES frame per vblank leaves the game 0.456% slow. Accumulate the shortfall
 * and run one extra NES frame whenever it reaches a whole frame — about every
 * 219 DS frames. That frame is emulated and drawn but never displayed, which
 * is the correct trade: game time matches the NES exactly and the dropped
 * frame is far below the threshold of perception.
 *   (60.0988 / 59.8261 - 1) * 2^24 = 76476 */
#define NES_RATE_FRAC  76476u
#define NES_RATE_ONE   (1u << 24)
static uint32_t s_rate_acc;
static int      s_catchup;


uint64_t g_nmi_count = 0;
static uint16_t s_last_pc;
static uint32_t s_defer_count;

/* SMB is vertically mirrored: $2000/$2800 are the same physical nametable,
 * as are $2400/$2C00. Masking to 0x7FF folds the mirrors onto the real pair.
 * (Horizontal-mirrored games would need (a & 0x3FF) | ((a >> 1) & 0x400).) */
#define NT_MIRROR 0x07FF

/* $3F10/$3F14/$3F18/$3F1C are the SAME bytes as $3F00/$3F04/$3F08/$3F0C —
 * sprite palette entry 0 and background entry 0 are one physical location. */
static inline uint8_t pal_index(uint16_t a) {
    uint8_t i = a & 0x1F;
    if ((i & 0x13) == 0x10) i &= ~0x10;   /* 10,14,18,1C -> 00,04,08,0C */
    return i;
}

#define CYCLES_PER_FRAME 29781
#define SPRITE0_CYC       5910   /* ~scanline 32: vblank + 32 scanlines */
static uint32_t s_ops;
static int      s_nmi_depth;

int nes_rom_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t hdr[16];
    if (fread(hdr, 1, 16, f) != 16 || memcmp(hdr, "NES\x1A", 4)) { fclose(f); return 0; }
    int prg16k = hdr[4], chr8k = hdr[5];
    if (prg16k == 1) {                   /* NROM-128: mirror 16K twice */
        fread(s_prg, 1, 0x4000, f);
        memcpy(s_prg + 0x4000, s_prg, 0x4000);
    } else {
        fread(s_prg, 1, 0x8000, f);
    }
    if (chr8k) fread(g_chr_ram, 1, 0x2000, f);
    fclose(f);

    return 1;
}

uint8_t nes_read(uint16_t addr) {
    if (addr < 0x2000) return g_ram[addr & 0x07FF];
    if (addr < 0x4000) {
        switch (addr & 7) {
        case 2: {                        /* PPUSTATUS: read clears vblank + toggle */
            uint8_t v = g_ppustatus;
            g_ppustatus &= ~0x80;
            s_wtoggle = 0;
            /* HACK: no dot-accurate PPU yet. SMB polls bit 6 twice per frame —
             * Sprite0Clr ($813D) waits for it LOW, Sprite0Hit ($8150) waits for
             * it HIGH. Derive it from position within the frame rather than a
             * read count, so it stays correct however often the game polls.
             * SPRITE0_CYC ~ the status-bar split at scanline 32:
             * 2273 vblank cycles + 32 * 113.67 cycles/scanline. */
            if (s_ops >= SPRITE0_CYC && s_ops < CYCLES_PER_FRAME - 1000)
                g_ppustatus |= 0x40;
            else
                g_ppustatus &= ~0x40;
            return v;
        }
        case 7: {                        /* PPUDATA */
            uint16_t a = s_ppuaddr & 0x3FFF;
            s_ppuaddr += (g_ppuctrl & 0x04) ? 32 : 1;
            if (a < 0x2000) return g_chr_ram[a];
            if (a < 0x3F00) return g_ppu_nt[a & NT_MIRROR];
            return g_ppu_pal[pal_index(a)];
        }
        default: return 0;
        }
    }
    if (addr == 0x4015) return apu_read_status();
    if (addr == 0x4016 || addr == 0x4017) {
        int p = addr & 1;
        uint8_t bit = s_ctrl_shift[p] & 1;
        s_ctrl_shift[p] >>= 1;
        return bit | 0x40;
    }
    if (addr >= 0x6000 && addr < 0x8000) return g_sram[addr - 0x6000];
    if (addr >= 0x8000) return s_prg[addr - 0x8000];
    return 0;
}

void nes_write(uint16_t addr, uint8_t val) {
    if (addr < 0x2000) { g_ram[addr & 0x07FF] = val; return; }
    if (addr < 0x4000) {
        switch (addr & 7) {
        case 0: g_ppuctrl = val; return;
        case 1: g_ppumask = val; return;
        case 3: g_oamaddr = val; return;
        case 4: g_ppu_oam[g_oamaddr++] = val; return;
        case 5:
            if (!s_wtoggle) g_ppuscroll_x = val; else g_ppuscroll_y = val;
            s_wtoggle ^= 1;
            return;
        case 6:
            if (!s_wtoggle) s_ppuaddr = (s_ppuaddr & 0x00FF) | ((uint16_t)val << 8);
            else            s_ppuaddr = (s_ppuaddr & 0xFF00) | val;
            s_wtoggle ^= 1;
            return;
        case 7: {
            uint16_t a = s_ppuaddr & 0x3FFF;
            if (a >= 0x2000 && a < 0x3F00)      g_ppu_nt[a & NT_MIRROR] = val;
            else if (a >= 0x3F00)               g_ppu_pal[pal_index(a)] = val;
            else if (!g_chr_is_rom)             g_chr_ram[a] = val;
            s_ppuaddr += (g_ppuctrl & 0x04) ? 32 : 1;
            return;
        }
        default: return;
        }
    }
    if (addr == 0x4014) {                /* OAM DMA */
        uint16_t base = (uint16_t)val << 8;
        for (int i = 0; i < 256; i++) g_ppu_oam[(uint8_t)(g_oamaddr + i)] = nes_read(base + i);
        s_ops += 513;
        return;
    }
    if (addr == 0x4016) {
        int was = s_ctrl_strobe;
        s_ctrl_strobe = val & 1;
        if (was && !s_ctrl_strobe) {
            s_ctrl_shift[0] = g_controller1_buttons;
            s_ctrl_shift[1] = g_controller2_buttons;
        }
        return;
    }
    if (addr >= 0x4000 && addr <= 0x4015) { apu_write(addr, val); return; }
    if (addr >= 0x6000 && addr < 0x8000) { g_sram[addr - 0x6000] = val; return; }
}

uint16_t nes_read16zp(uint8_t zp) {
    return (uint16_t)g_ram[zp] | ((uint16_t)g_ram[(uint8_t)(zp + 1)] << 8);
}

/* The NES shift register clocks out A first, then B, Select, Start, Up,
 * Down, Left, Right. nes_read() shifts right and returns bit 0, so A must
 * sit in bit 0 and Right in bit 7 — not the other way round. */
static void poll_input(void) {
    scanKeys();
    int k = keysHeld();
    uint8_t b = 0;
    if (k & KEY_A)      b |= 0x01;   /* NES A — jump */
    if (k & KEY_B)      b |= 0x02;   /* NES B — run/fire */
    if (k & KEY_SELECT) b |= 0x04;
    if (k & KEY_START)  b |= 0x08;
    if (k & KEY_UP)     b |= 0x10;
    if (k & KEY_DOWN)   b |= 0x20;
    if (k & KEY_LEFT)   b |= 0x40;
    if (k & KEY_RIGHT)  b |= 0x80;
    g_controller1_buttons = b;

    if (keysDown() & KEY_L) {
        s_debug_on = !s_debug_on;
        if (!s_debug_on) iprintf("\x1b[2J");   /* clear on the way out */
    }
}

void nes_timing_init(void) { timing_init(); }

void maybe_trigger_vblank(int cycles) {
    s_ops += (cycles > 0) ? (uint32_t)cycles : 1;
    if (s_ops < CYCLES_PER_FRAME) return;

    if (s_nmi_depth) {
        /* Still inside func_NMI a whole frame later — it isn't returning.
         * Report where the 6502 is spinning instead of hanging silently. */
        s_ops -= CYCLES_PER_FRAME;
        if ((++s_defer_count % 30) == 1)
            iprintf("in NMI %lu frames, pc=%04X A=%02X X=%02X Y=%02X\n",
                    (unsigned long)s_defer_count, s_last_pc,
                    g_cpu.A, g_cpu.X, g_cpu.Y);
        return;
    }

    s_ops -= CYCLES_PER_FRAME;
    g_ppustatus |= 0x80;                 /* vblank starts */
    g_ppustatus &= ~0x40;                /* sprite 0 hit clears each frame */

    poll_input();
    uint16_t t_start = tmr();
    /* Frame period must be measured start-to-start. Measuring from here to
     * the end of video_flush misses the tail of the frame — the recompiled
     * main loop still running until the next budget expiry — which made the
     * period look short and fps read high. */
    if (s_have_last) s_total_acc += (uint16_t)(t_start - s_last_start);
    s_last_start = t_start;
    s_have_last  = 1;

    if (g_ppuctrl & 0x80) {
        s_nmi_depth++;
        g_nmi_count++;
        /* Push the 6502 NMI hardware frame: PC high, PC low, status. The
         * generated func_NMI() ends in RTI, which pops all three — without
         * this push the stack pointer climbs by 3 every frame until a PHA
         * lands on the PPUCTRL byte the handler saved, which is what was
         * producing ctrl=0x8B and the flashing. $0000 is the sentinel PC the
         * recompiler expects; RTI reports any hijacked target separately. */
        g_ram[0x100 + g_cpu.S--] = 0x00;   /* PC high */
        g_ram[0x100 + g_cpu.S--] = 0x00;   /* PC low  */
        g_ram[0x100 + g_cpu.S--] =
            (uint8_t)((g_cpu.N << 7) | (g_cpu.V << 6) | 0x20 |
                      (g_cpu.D << 3) | (g_cpu.I << 2) |
                      (g_cpu.Z << 1) |  g_cpu.C);
        func_NMI();
        s_nmi_depth--;
    }

    /* Wait until vblank BEFORE touching VRAM. video_frame() rewrites the whole
     * BG map and all of OAM; doing that while the display is scanning them out
     * tears and flickers. func_NMI() is most of a frame's work, so waiting
     * first (as this used to) put the writes right in the middle of display. */
    video_build();          /* heavy work: RAM shadows, outside vblank */
    apu_frame();

    /* Work time excludes the vblank wait: it is what we spend emulating and
     * building the frame, so anything approaching 16.7ms means dropped frames. */
    s_work_acc += (uint16_t)(tmr() - t_start);

    /* Skip the wait on a catch-up frame so two NES frames land inside one
     * DS frame; the first one's output is simply overwritten. */
    if (s_catchup) s_catchup = 0;
    else           swiWaitForVBlank();

    video_flush();

    s_rate_acc += NES_RATE_FRAC;
    if (s_rate_acc >= NES_RATE_ONE) {
        s_rate_acc -= NES_RATE_ONE;
        s_catchup = 1;
    }
    /* Do NOT clear the vblank flag here — on hardware it stays set until the
     * CPU reads $2002, and nes_read() does that. Clearing it here means the
     * RESET spin-wait at $800A never sees it and the game never starts. */

    if ((++g_frame_count % 60) == 0) {
        /* Average first, then scale — 60 * 65535 * 3052 overflows 32 bits. */
        /* Average first, then scale. fps is kept x100: integer 1000000/us
         * lands right on the 59/60 boundary and flips with rounding, which
         * looks like instability when the timing is actually fine. */
        uint32_t frame_us = ticks_to_us(s_total_acc / 60u);
        s_work_us = ticks_to_us(s_work_acc  / 60u);
        s_fps     = frame_us ? (100000000u / frame_us) : 0;
        s_work_acc = s_total_acc = 0;

        if (s_debug_on) {
            /* Home the cursor instead of scrolling — scrolling the console is
             * itself slow enough to skew the numbers. */
            iprintf("\x1b[0;0Hfps %2lu.%02lu  work %2lu.%02lums   \n",
                    (unsigned long)(s_fps / 100), (unsigned long)(s_fps % 100),
                    (unsigned long)(s_work_us / 1000),
                    (unsigned long)((s_work_us % 1000) / 10));
            iprintf("om=%02X ot=%02X srt=%02X ges=%02X  \n",
                    g_ram[0x0770], g_ram[0x0772], g_ram[0x073C], g_ram[0x000E]);
            /* Should read ~60.1, not 59.8, once rate matching is working. */
        }
    }
}

void nes_instruction_boundary(uint16_t pc, int cycles) {
    s_last_pc = pc; maybe_trigger_vblank(cycles);
}
void nes_cpu_instruction_boundary(uint16_t pc, int cycles) {
    s_last_pc = pc; maybe_trigger_vblank(cycles);
}
int  coroutine_scheduler_setjmp(void)                      { return 0; }
