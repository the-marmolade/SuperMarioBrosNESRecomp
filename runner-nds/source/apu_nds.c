/* apu_nds.c — NES APU on DS sound hardware.
 *
 * The DS has PSG square channels with selectable duty (8-13) and noise
 * channels (14-15), which is very close to what the NES APU produces, so the
 * two pulse channels and the noise channel map almost directly. There is no
 * hardware triangle, so that one plays a 32-step triangle wavetable as a
 * looping sample — which is exactly what the NES triangle is anyway.
 *
 * Frequency units, worked out from the DS sound timer:
 *   PSG/noise: pass 8 * desired_Hz
 *   PCM:       pass the sample rate, i.e. 32 * triangle_Hz for a 32-step wave
 *
 * Not implemented yet: DMC (SMB doesn't use it) and the 5-step frame
 * sequencer mode (SMB uses 4-step).
 */
#include <nds.h>
#include <string.h>
#include "nes_runtime.h"

#define NES_CPU_HZ 1789773

static const uint8_t s_len_table[32] = {
    10,254, 20,  2, 40,  4, 80,  6,160,  8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22,192, 24, 72, 26, 16, 28, 32, 30
};

/* NTSC noise period table, in CPU cycles. */
static const uint16_t s_noise_period[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068
};

/* NES duty 0,1,2,3 = 12.5%, 25%, 50%, 25%-negated.
 * DS duty 0..7 = 12.5%, 25%, 37.5%, 50%, ... — so 50% is index 3. */
static const uint8_t s_duty_map[4] = { 0, 1, 3, 1 };

typedef struct {
    uint8_t reg[4];
    int     enabled;
    int     length;
    int     env_start, env_div, env_decay;
    int     sweep_div;
    int     timer;          /* 11-bit period */
} Chan;

static Chan s_p1, s_p2, s_noise;
static struct {
    uint8_t reg[4];
    int enabled, length, lin_counter, lin_reload, lin_reload_flag, timer;
} s_tri;

static int s_seq_step;
static int s_ch_p1 = -1, s_ch_p2 = -1, s_ch_tri = -1, s_ch_nz = -1;
static int8_t s_tri_wave[32];
static int s_ready;

/* ---------- register writes ---------- */

static void chan_write(Chan *c, int idx, uint8_t val) {
    c->reg[idx] = val;
    if (idx == 3) {                     /* length load + envelope restart */
        c->length    = s_len_table[(val >> 3) & 0x1F];
        c->env_start = 1;
    }
}

void apu_write(uint16_t addr, uint8_t val) {
    switch (addr) {
    case 0x4000: case 0x4001: case 0x4002: case 0x4003:
        chan_write(&s_p1, addr - 0x4000, val); break;
    case 0x4004: case 0x4005: case 0x4006: case 0x4007:
        chan_write(&s_p2, addr - 0x4004, val); break;
    case 0x4008:
        s_tri.reg[0] = val; s_tri.lin_reload = val & 0x7F; break;
    case 0x400A:
        s_tri.reg[2] = val; break;
    case 0x400B:
        s_tri.reg[3] = val;
        s_tri.length = s_len_table[(val >> 3) & 0x1F];
        s_tri.lin_reload_flag = 1;
        break;
    case 0x400C: case 0x400E: case 0x400F:
        chan_write(&s_noise, addr - 0x400C, val); break;
    case 0x4015:
        s_p1.enabled    = val & 1;
        s_p2.enabled    = val & 2;
        s_tri.enabled   = val & 4;
        s_noise.enabled = val & 8;
        if (!s_p1.enabled)    s_p1.length = 0;
        if (!s_p2.enabled)    s_p2.length = 0;
        if (!s_tri.enabled)   s_tri.length = 0;
        if (!s_noise.enabled) s_noise.length = 0;
        break;
    default: break;
    }
}

uint8_t apu_read_status(void) {
    return (uint8_t)((s_p1.length    ? 1 : 0) |
                     (s_p2.length    ? 2 : 0) |
                     (s_tri.length   ? 4 : 0) |
                     (s_noise.length ? 8 : 0));
}

/* ---------- frame sequencer ---------- */

static void clock_envelope(Chan *c) {
    if (c->env_start) {
        c->env_start = 0;
        c->env_decay = 15;
        c->env_div   = c->reg[0] & 0x0F;
    } else if (--c->env_div < 0) {
        c->env_div = c->reg[0] & 0x0F;
        if (c->env_decay > 0)          c->env_decay--;
        else if (c->reg[0] & 0x20)     c->env_decay = 15;   /* loop */
    }
}

static void clock_length(Chan *c) {
    if (c->length > 0 && !(c->reg[0] & 0x20)) c->length--;
}

static void clock_sweep(Chan *c, int is_pulse1) {
    uint8_t s = c->reg[1];
    if (!(s & 0x80)) return;                       /* sweep disabled */
    if (--c->sweep_div >= 0) return;
    c->sweep_div = (s >> 4) & 7;

    int period = ((c->reg[3] & 7) << 8) | c->reg[2];
    int delta  = period >> (s & 7);
    if (s & 0x08) period -= delta + (is_pulse1 ? 1 : 0);
    else          period += delta;
    if (period < 8 || period > 0x7FF) return;

    c->reg[2] = (uint8_t)(period & 0xFF);
    c->reg[3] = (uint8_t)((c->reg[3] & ~7) | ((period >> 8) & 7));
}

static int chan_volume(const Chan *c) {
    if (!c->length) return 0;
    return (c->reg[0] & 0x10) ? (c->reg[0] & 0x0F) : c->env_decay;
}

/* ---------- pushing state to the DS ---------- */

static void push_pulse(Chan *c, int ch, int is_p1) {
    int period = ((c->reg[3] & 7) << 8) | c->reg[2];
    int vol    = chan_volume(c);
    (void)is_p1;

    /* The sweep unit mutes the channel when the period is below 8 or when the
     * computed target would overflow 11 bits — not just when sweep is on. */
    int target = period + (period >> (c->reg[1] & 7));
    if (period < 8 || target > 0x7FF || vol == 0) { soundSetVolume(ch, 0); return; }

    int hz = NES_CPU_HZ / (16 * (period + 1));
    /* freq param is 8*Hz in a u16, so the ceiling is 8191Hz. SMB's melody
     * stays well below that; anything higher is a sweep sliding off the top
     * and is meant to be inaudible anyway. */
    if (hz < 20 || hz > 8191) { soundSetVolume(ch, 0); return; }

    soundSetFreq(ch, (u16)(hz * 8));
    soundSetVolume(ch, (u8)(vol * 85 / 15));
}

static void push_triangle(int ch) {
    int period = ((s_tri.reg[3] & 7) << 8) | s_tri.reg[2];
    if (!s_tri.length || !s_tri.lin_counter || period < 2) {
        soundSetVolume(ch, 0);
        return;
    }
    int hz = NES_CPU_HZ / (32 * (period + 1));
    if (hz < 20 || hz > 2000) { soundSetVolume(ch, 0); return; }

    soundSetFreq(ch, (u16)(hz * 32));     /* PCM: freq param is the sample rate */
    soundSetVolume(ch, 65);               /* NES triangle has no volume control */
}

/* The PSG square divides its timer by 8 to build the duty cycle, so it wants
 * 8 * Hz. The noise LFSR advances once per timer tick, so it wants the clock
 * rate directly. Passing 8x here made every drum hiss at the same pitch.
 * If percussion still sounds wrong, try 8 — this is the one scaling factor
 * I'm least sure of. */
#define NOISE_SCALE 1

static void push_noise(int ch) {
    int vol = chan_volume(&s_noise);
    if (!vol) { soundSetVolume(ch, 0); return; }

    int hz = NES_CPU_HZ / s_noise_period[s_noise.reg[2] & 0x0F];
    hz *= NOISE_SCALE;
    if (hz > 65535) hz = 65535;
    soundSetFreq(ch, (u16)hz);
    soundSetVolume(ch, (u8)(vol * 60 / 15));
}

/* ---------- public entry points ---------- */

void apu_init(void) {
    /* 32-step triangle: 0..15 then 15..0, as signed 8-bit. */
    for (int i = 0; i < 32; i++) {
        int step = (i < 16) ? i : (31 - i);
        s_tri_wave[i] = (int8_t)((step - 8) * 16);
    }

    soundEnable();
    /* Start every channel silent and keep it alive; per-frame updates then
     * only change frequency and volume, which is far cheaper than starting
     * and stopping channels. */
    s_ch_p1  = soundPlayPSG(DutyCycle_50, 440 * 8, 0, 64);
    s_ch_p2  = soundPlayPSG(DutyCycle_50, 440 * 8, 0, 64);
    s_ch_nz  = soundPlayNoise(1000 * 8, 0, 64);
    s_ch_tri = soundPlaySample(s_tri_wave, SoundFormat_8Bit, sizeof s_tri_wave,
                               440 * 32, 0, 64, true, 0);
    s_ready = 1;
}

/* Called once per NES frame. The APU frame sequencer runs at 240Hz, so step
 * it four times per video frame: envelopes every step, length counters and
 * sweep on steps 1 and 3 (the half-frames). */
void apu_frame(void) {
    if (!s_ready) return;

    for (int i = 0; i < 4; i++) {
        clock_envelope(&s_p1);
        clock_envelope(&s_p2);
        clock_envelope(&s_noise);
        /* Linear counter: reload if the flag is set, otherwise count down.
         * The flag is only cleared when the control bit is clear, which is
         * what lets the game hold a note indefinitely. */
        if (s_tri.lin_reload_flag)      s_tri.lin_counter = s_tri.lin_reload;
        else if (s_tri.lin_counter > 0) s_tri.lin_counter--;
        if (!(s_tri.reg[0] & 0x80))     s_tri.lin_reload_flag = 0;

        if ((s_seq_step & 1) == 1) {
            clock_length(&s_p1);
            clock_length(&s_p2);
            clock_length(&s_noise);
            if (s_tri.length > 0 && !(s_tri.reg[0] & 0x80)) s_tri.length--;
            clock_sweep(&s_p1, 1);
            clock_sweep(&s_p2, 0);
        }
        s_seq_step = (s_seq_step + 1) & 3;
    }

    /* Duty can change between frames, and there is no setter for it, so the
     * PSG channels are restarted when it does. */
    static uint8_t last_d1 = 0xFF, last_d2 = 0xFF;
    uint8_t d1 = s_duty_map[(s_p1.reg[0] >> 6) & 3];
    uint8_t d2 = s_duty_map[(s_p2.reg[0] >> 6) & 3];
    if (d1 != last_d1) { soundKill(s_ch_p1); s_ch_p1 = soundPlayPSG(d1, 440 * 8, 0, 64); last_d1 = d1; }
    if (d2 != last_d2) { soundKill(s_ch_p2); s_ch_p2 = soundPlayPSG(d2, 440 * 8, 0, 64); last_d2 = d2; }

    push_pulse(&s_p1, s_ch_p1, 1);
    push_pulse(&s_p2, s_ch_p2, 0);
    push_triangle(s_ch_tri);
    push_noise(s_ch_nz);
}
