#include <nds.h>
#include <fat.h>
#include <filesystem.h>
#include <stdio.h>
#include <stdint.h>

extern void func_RESET(void);
extern void func_NMI(void);
extern void func_IRQ(void);
extern int  call_by_address(uint16_t addr);
extern int  nes_rom_load(const char *path);
extern void video_init(void);
extern void nes_timing_init(void);
extern void apu_init(void);

static void *const g_entry_anchors[] = {
    (void *)func_RESET, (void *)func_NMI,
    (void *)func_IRQ,   (void *)call_by_address,
};

int main(void) {
    consoleDemoInit();

    volatile uintptr_t sink = 0;
    for (unsigned i = 0; i < sizeof(g_entry_anchors)/sizeof(*g_entry_anchors); i++)
        sink += (uintptr_t)g_entry_anchors[i];

    /* NitroFS first: the ROM is embedded in the .nds, so no SD card or DLDI
     * patching is needed and it works identically in DeSmuME, melonDS and on
     * hardware. Falls back to the SD card if the build has no nitrofiles. */
    if (nitroFSInit(NULL) && nes_rom_load("nitro:/smb.nes")) {
        iprintf("ROM loaded from NitroFS\n");
    } else if (fatInitDefault() &&
               (nes_rom_load("/smb.nes") || nes_rom_load("fat:/smb.nes"))) {
        iprintf("ROM loaded from SD\n");
    } else {
        iprintf("could not load smb.nes\n");
        while (1) swiWaitForVBlank();
    }
    video_init();
    nes_timing_init();
    apu_init();
    iprintf("press L for fps/debug overlay\n");

    func_RESET();
    iprintf("RESET returned (unexpected)\n");
    while (1) swiWaitForVBlank();
}
