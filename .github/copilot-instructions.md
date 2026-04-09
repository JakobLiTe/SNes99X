# Snes9x Development Guide

Snes9x is a portable Super Nintendo Entertainment System (SNES) emulator. This guide covers the architecture, build systems, and conventions specific to this codebase.

## Build Systems

### Platform-Specific Builds

Each platform has its own build system:

- **GTK (Linux)**: `gtk/CMakeLists.txt` - CMake-based build
  ```bash
  cd gtk
  cmake -B build
  cmake --build build
  ```

- **Unix (X11)**: `unix/configure.ac` - Autotools-based build
  ```bash
  cd unix
  ./configure
  make
  ```

- **Qt**: `qt/CMakeLists.txt` - CMake-based build
  ```bash
  cd qt
  cmake -B build
  cmake --build build
  ```

- **macOS**: `macosx/snes9x.xcodeproj` - Xcode project

- **Windows**: `win32/snes9xw.sln` - Visual Studio solution

- **libretro**: `libretro/Makefile` - Makefile-based build for RetroArch core
  ```bash
  cd libretro
  make
  ```

### Common Compile-Time Options

Defined in individual build systems or passed as flags:

- `RIGHTSHIFT_IS_SAR` - Define if compiler uses shift right arithmetic (GCC, MSVC)
- `ZLIB` - Enable GZIP compression support
- `UNZIP_SUPPORT` - Enable ZIP compressed ROM support
- `JMA_SUPPORT` - Enable JMA archive support
- `DEBUGGER` - Enable debugging features (slows emulation)
- `NETPLAY_SUPPORT` - Enable network play features
- `USE_OPENGL` - Enable OpenGL rendering
- `ALLOW_CPU_OVERCLOCK` - Allow CPU overclocking options

### Pixel Format Configuration

Edit `port.h` based on target platform:

- Define `LSB_FIRST` for little-endian systems
- Set `PIXEL_FORMAT` to `RGB565`, `RGB555`, `BGR565`, or `BGR555`
- Or define `GFX_MULTI_FORMAT` and use `S9xSetRenderPixelFormat()` for dynamic switching

## Architecture Overview

### Core Components

Snes9x emulates multiple SNES subsystems simultaneously:

- **CPU**: 65c816 processor emulation (`cpu.cpp`, `cpuexec.cpp`, `cpuops.cpp`)
- **PPU**: Picture Processing Unit for graphics (`ppu.cpp`, `gfx.cpp`, `tile.cpp`)
- **APU**: Audio Processing Unit (`apu/` directory with BAPU implementation)
- **DMA**: Direct Memory Access (`dma.cpp`)
- **Memory Mapping**: ROM/RAM address mapping (`memmap.cpp`, `getset.h`)

### Special Chips / Coprocessors

Many SNES cartridges included additional chips. Each has dedicated implementation files:

- **SA-1**: Fast coprocessor (`sa1.cpp`, `sa1cpu.cpp`)
- **SuperFX**: 3D graphics chip (`fxemu.cpp`, `fxinst.cpp`)
- **DSP-1/2/3/4**: Math coprocessors (`dsp1.cpp` - `dsp4.cpp`)
- **C4**: Sprite transformation (`c4.cpp`, `c4emu.cpp`)
- **SDD-1**: Decompression chip (`sdd1.cpp`, `sdd1emu.cpp`)
- **SPC7110**: Decompression chip (`spc7110.cpp`, `spc7110emu.cpp`)
- **OBC1**: Memory controller (`obc1.cpp`)
- **S-RTC**: Real-time clock (`srtc.cpp`, `srtcemu.cpp`)
- **ST010/ST011/ST018**: Seta chips (`seta010.cpp`, `seta011.cpp`, `seta018.cpp`)
- **BS-X**: Satellaview support (`bsx.cpp`)
- **MSU-1**: Audio streaming (`msu1.cpp`)

### Memory Organization

The SNES has a complex memory map. Key memory areas in `memmap.h`:

- `RAM[0x20000]` - Internal WRAM
- `VRAM[0x10000]` - Video RAM
- `SRAM` - Save RAM (battery-backed)
- `ROM` - Cartridge ROM
- `FillRAM` - I/O registers and other special memory
- `Map[MEMMAP_NUM_BLOCKS]` - Address space mapping table
- `WriteMap[MEMMAP_NUM_BLOCKS]` - Write mapping table

Memory access is handled through `getset.h` macros that account for:
- Memory access speed (fast/slow ROM regions)
- Special chip memory ranges
- CPU cycle counting via `addCyclesInMemoryAccess`

### Input System

Controller emulation is handled through a flexible mapping system:

1. Call `S9xUnmapAllControl()` during initialization
2. Map IDs to SNES buttons using `S9xMapButton()` or pointers using `S9xMapPointer()`
3. Call `S9xReportButton()` before `S9xMainLoop()` for joypad buttons
4. Implement `S9xPollButton()` and `S9xPollPointer()` for polled devices

Supported devices (via `S9xSetController()`):
- `CTL_JOYPAD` - Standard controller
- `CTL_MOUSE` - SNES Mouse
- `CTL_SUPERSCOPE` - Super Scope light gun
- `CTL_JUSTIFIER` - Justifier light gun
- `CTL_MP5` - Multi Player 5 adapter (5-player support)

See `controls.h`, `controls.txt`, and `control-inputs.txt` for full details.

### Rendering Pipeline

The PPU renders scanline-by-scanline:

1. Background layers processed in `ppu.cpp`
2. Tile rendering in `tile.cpp` with specializations (`tileimpl-*.cpp`)
3. Sprites (OBJ) handled in PPU
4. Effects (transparency, color math) applied
5. Output to 16-bit `ScreenBuffer` in RGB565 (or configured format)
6. Platform-specific display drivers handle final output

Screen dimensions:
- `SNES_WIDTH` = 256 pixels
- `SNES_HEIGHT` = 224 pixels (NTSC) or 239 (extended)
- `MAX_SNES_WIDTH/HEIGHT` = 2x for hi-res modes

### Snapshot/Save State System

- `snapshot.cpp` - Save/load full emulator state
- `statemanager.cpp` - Manages multiple save slots
- S-RAM auto-save for battery-backed games
- Freeze files are 400K+, optionally GZIP compressed with zlib

## Key Conventions

### Timing and Cycles

- All timing based on master clock rates (NTSC: 21.477 MHz, PAL: 21.281 MHz)
- CPU cycle counting critical for accuracy
- `ONE_CYCLE` = 6 master clock cycles (or configurable with `ALLOW_CPU_OVERCLOCK`)
- `SLOW_ONE_CYCLE` = 8 master clock cycles
- `ONE_DOT_CYCLE` = 4 master clock cycles

### Platform Abstraction

Platform-specific code goes in dedicated directories:
- `gtk/` - GTK+ interface
- `unix/` - X11 interface  
- `macosx/` - macOS Cocoa interface
- `win32/` - Windows interface
- `qt/` - Qt interface
- `libretro/` - RetroArch core

Core emulation files at root level should remain platform-independent.

### Type Definitions

Use types from `port.h`:
- `uint8`, `uint16`, `uint32`, `uint64` for unsigned integers
- `int8`, `int16`, `int32`, `int64` for signed integers
- `bool8` for boolean values

### File I/O Abstraction

Use FSTREAM/STREAM macros (from `snes9x.h`) instead of direct file I/O:
- Abstracts zlib compression when `ZLIB` is defined
- `OPEN_FSTREAM()`, `READ_FSTREAM()`, `WRITE_FSTREAM()`, etc.
- Stream class in `stream.h` for object-oriented file access

### Emulation Loop

Main loop is in `S9xMainLoop()` (`cpuexec.cpp`):
1. Check/report input state
2. Execute CPU instructions
3. Process DMA/HDMA events
4. Render scanlines
5. Generate audio samples
6. Handle timing synchronization

Platform ports call `S9xMainLoop()` repeatedly and handle:
- Display output
- Sound output
- Input polling
- Timing/throttling

## Documentation

- `docs/porting.html` - Comprehensive porting guide
- `docs/controls.txt` - Input system documentation
- `docs/control-inputs.txt` - Control mapping details
- GitHub Wiki - Additional resources and guides
- `docs/snapshots.txt` - Save state format

## Continuous Integration

- Windows builds: AppVeyor (`appveyor.yml`)
- Unix/macOS/libretro builds: Cirrus CI (`.cirrus.yml`)
- Nightly builds available for all platforms

## Cycle-Accurate Scheduler (New)

### Overview

The codebase now includes a master-clock-based scheduler for improved timing accuracy. This scheduler coordinates all emulation components (CPU, PPU, APU, DMA) at the 21.477 MHz master clock level.

### Key Files

- `scheduler.h` / `scheduler.cpp` - Event-driven scheduler implementation
- Modified: `cpu.cpp` - Initializes scheduler during reset
- Modified: `cpuexec.cpp` - Main loop integration with per-instruction timing

### Architecture

```cpp
class Scheduler {
    void addEvent(int64 timestamp, ...);        // Absolute time
    void addEventRelative(int64 cycles, ...);   // Relative time
    void advanceTo(int64 targetClock);          // Process events
    int64 cpuCyclesToMaster(int32);             // Convert cycles
};
```

### Integration Points

1. **Initialization**: `S9xInitScheduler()` called in `S9xReset()`
2. **Per-Instruction Timing**: Main loop captures cycles before/after each CPU instruction
3. **Event Callbacks**: Horizontal events (HBLANK, HDMA, Render) registered per scanline
4. **Master Clock**: All timing based on 21.477 MHz (NTSC) or 21.281 MHz (PAL)

### Event Types

- `EVENT_HBLANK_START` - Horizontal blank
- `EVENT_HDMA_START` - HDMA transfer
- `EVENT_HCOUNTER_MAX` - End of scanline
- `EVENT_HDMA_INIT` - HDMA initialization
- `EVENT_RENDER` - Scanline rendering
- `EVENT_WRAM_REFRESH` - WRAM refresh cycles

### Conversion Constants

- `ONE_CYCLE` = 6 master clocks (1 CPU cycle)
- `SLOW_ONE_CYCLE` = 8 master clocks
- `ONE_DOT_CYCLE` = 4 master clocks

### Build Integration

The scheduler is integrated into all build systems:
- **libretro**: `libretro/Makefile.common`
- **GTK**: `gtk/CMakeLists.txt`
- **Unix**: `unix/Makefile.in`
- **Qt**: Add `scheduler.cpp` to sources
- **Windows**: Add to Visual Studio project
- **macOS**: Add to Xcode project

