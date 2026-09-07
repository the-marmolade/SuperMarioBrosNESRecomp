/* Placeholder implementations — enough to link and run the recompiled game.
 * Several of these need real behaviour eventually; see notes below. */
#include <stdint.h>
#include <stdio.h>
#include "nes_runtime.h"

/* Defined in the generated dispatch TU. */
extern int call_by_address_cb(uint16_t addr, int caller_bank);

int g_current_bank = 0;

int nes_dispatch_call(uint16_t addr, int caller_bank) {
    uint16_t save_wb = g_code_window_base;
    if (addr >= 0x8000) g_code_window_base = addr & 0xE000;
    int r = call_by_address_cb(addr, caller_bank);
    g_code_window_base = save_wb;
    return r;
}

/* Recursive tail dispatch. Upstream detects JMP cycles and flattens them;
 * without that, a tail loop will grow the C stack. */
int call_by_address_tail(uint16_t addr, int caller_bank) {
    return nes_dispatch_call(addr, caller_bank);
}

/* NMOS 6502 JMP (abs) page-wrap erratum: the high-byte fetch stays inside
 * the same 256-byte page as the low byte. $12FF reads hi from $1200. */
uint16_t nes_read16_jmpbug(uint16_t addr) {
    uint8_t lo = nes_read(addr);
    uint8_t hi = nes_read((addr & 0xFF00) | (uint16_t)((addr + 1) & 0x00FF));
    return (uint16_t)lo | ((uint16_t)hi << 8);
}

uint8_t game_ram_read_hook(uint16_t pc, uint16_t addr, uint8_t val) {
    (void)pc; (void)addr; return val;
}

/* No 6502 interpreter in this build. Addresses reaching these were never
 * statically recompiled, so the call is silently skipped. This is why SMB's
 * JumpEngine paths misbehave — porting runner/src/interp.c is the real fix,
 * but it needs the mapper wired up properly to be worth doing. */
int nes_interp_dispatch(uint16_t addr) { (void)addr; return 0; }
int nes_interp_dispatch_bank(uint16_t cpu_addr, uint16_t gen_addr, int bank) {
    (void)cpu_addr; (void)gen_addr; (void)bank; return 0;
}
int nes_interp_force_generated(uint16_t gen_addr, int bank) {
    (void)gen_addr; (void)bank; return 0;
}

void nes_brk_executed(uint16_t pc) { (void)pc; }
void nes_log_inline_miss(uint16_t dispatch_pc, uint8_t a_val) { (void)dispatch_pc; (void)a_val; }
int  nes_mod_function_entry(uint16_t addr) { (void)addr; return 0; }
void runtime_begin_post_nmi(void) { }
void runtime_end_post_nmi(void)   { }
