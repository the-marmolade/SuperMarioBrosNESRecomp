target remote localhost:3333
set confirm off
set pagination off

define nesstate
  printf "ctrl=%02X mask=%02X status=%02X scroll=%d frame=%llu\n", \
    g_ppuctrl, g_ppumask, g_ppustatus, g_ppuscroll_x, g_frame_count
  printf "om=%02X ot=%02X srt=%02X ges=%02X\n", \
    g_ram[0x0770], g_ram[0x0772], g_ram[0x073C], g_ram[0x000E]
end

define hud
  printf "nt row3: "
  output *(unsigned char[32]*)&g_ppu_nt[0x060]
  printf "\n"
end
