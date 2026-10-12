# WinAPE 2.0b2 feature inventory (target for MacAPE)

Source: REA analysis of `WinApe.exe` (Delphi/VCL, 46 forms, DirectDraw/DirectSound), its help CHM and
resources. Menus/forms decoded from the PE `RCDATA` TPF0 resources.

## Emulated hardware
- Z80 with CPC NOP timing (tables recovered from exe, `tools/extract_z80_timing.py`); Turbo mode (1us/instr, OUT 2us).
- CRTC 6845 types 0-4 (selectable), Gate Array (modes 0-2, palette, 52-line IRQ divider, ROM/RAM banking).
- PPI 8255 (+ "Plus PPI" variant), AY-3-8912 PSG (3ch mono/stereo, 22/44 kHz, 8/16 bit), tape motor/relay.
- FDC 765 with fast-disc option (0.4 ms step), up to 4 drives, ParaDOS DDB, single-sided-read, temp writes.
- Amstrad Plus ASIC: 4096-colour palette, 16 hardware sprites, soft-scroll, split screen, programmable raster
  IRQ, 3 DMA sound channels, cartridge (.CPR RIFF / .CRT), secondary ROM mapping, unlock sequence.
- Expansion RAM: 64K / 128K / 64K+256K / 256K Silicon Disc / Yarek 4 MB; 32 upper ROM slots; Multiface II.
- Peripherals: AMX mouse, joystick, Symbiface II (IDE master/slave, RTC, PS/2 mouse), AmDrum, Digiblaster,
  printer (disabled/printer/file/assembler tab), Dobbertin SmartWatch ROM, drive sounds, tape sounds.

## Media / file formats
DSK, EDSK, DSC/HDR, ARC (disc); CDT, TZX, VOC, WAV, CSV (tape; WAV recording); SNA v1/v2/v3 (+RLE chunks);
SNP/SNR session recording; CPR/CRT cartridges; ROM images; PNG/BMP screenshots; AVI/WAV/YM recording;
WPF profiles (INI), KBD keyboard maps, POK pokes, symbol files, Auto-Type files, ZIP/RAR containers, IDE images.

## UI
Main window (control panel: run/step/step-over/pause/registers/assembler/disc/cart/tape/snap load/save;
status: settings, pokes, fullscreen, help, drive LEDs, track numbers, playback timer), menus File/Settings/Debug/
Assembler/Help, right-click menu, full screen (separate options), half size, render-both-lines, stretch, PAL
emulation, linear palette, colour/green/grey monitor, brightness, V-hold, speed 5%-1000%, frame skip.
Settings tabs: General, Display, Sound, Memory(ROMs), Input (key map 3 keys x NumLock states, joystick), Other.

## Debugger
Registers (Z80 + flags + stack + T counter), disassembler pane (follow PC, run to cursor, data areas),
memory dump pane (read/write/any mapping, ROM/RAM bank selection), find/goto/fill/compare/load/save,
breakpoints (code/memory R/W/IO, conditions with registers, variables, functions, pass counts, BRK ED FF,
timers), chip register window (palette, PSG, CRTC, VCC/VLC/R52/VSC/HDC/VTAC/HCC/HSC/VMA/VDUR/ICSR, ASIC
sprites/DMA/PRI/SSS/LRB/cart bank), Find Graphics (tile viewer/editor), symbols.

## Tools (later change sets)
Z80 assembler (Maxam-compatible, macros, directives, breakpoints in source), disc/file editor and sector editor,
format disc, pokes manager, Auto-Type/Paste, AVI/WAV/YM recorders, session record/playback, command line
(`disc`, `/A[:prog]`, `/T:`, `/SN:`, `/SYM:`).
