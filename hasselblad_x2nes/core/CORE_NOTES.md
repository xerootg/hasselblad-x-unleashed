# LaiNES core porting notes (x2nes / Hasselblad X2D II platform)

> Automatically compiled from source inspection, this document records the LaiNES version, file structure,
> all interface coupling points between the core and SDL2 platform layer, mapper support, and build considerations.
> Goal: reuse the pure core with our own platform layer (Android NDK, libc++_static), **without changing the LaiNES core source itself**.

## 1. Version and source

- Repository: https://github.com/AndreaOrru/LaiNES (master branch)
- Obtained via: `git clone --depth 1`
- Commit: `1f3d126171104ec57e515641ea3b5291d383ebdf` (short `1f3d126`, 2025-11-05, "Updated README")
- Local path: `C:\Users\tom\Documents\test\x2nes\core\LaiNES\`
- Upstream build system: SCons (`SConstruct`), clang++, `-std=c++14 -O3 -march=native`

### Licenses (important: differ slightly from the task assumptions)

| Component | License |
|---|---|
| LaiNES itself (src/, `LICENSE` file) | **BSD 2-Clause** (Copyright (c) 2020, Andrea Orru). Note: not MIT—the task description says MIT, but the actual LICENSE file is BSD-2. Both allow closed-source ports, but copyright notices and disclaimers must be retained. |
| `lib/` (blargg Nes_Snd_Emu 0.1.7, Shay Green) | **GNU LGPL** (see the header of `lib/include/Nes_Apu.h`). Static linking into an APK/firmware requires LGPL compliance (provide relinkable object files or source). This must be considered when deciding on the port. |
| `simpleini/` (git submodule, brofield/simpleini) | MIT. The submodule is not initialized in this clone (empty directory); only `src/config.cpp` uses it, and config belongs to the platform layer being replaced, so it can be discarded entirely. |

## 2. File inventory and classification

### 2.1 Pure core (no SDL dependency, reusable directly)

| File | Lines | Description |
|---|---|---|
| `src/cpu.cpp` + `src/include/cpu.hpp` | 908 + 41 | 6502 CPU, including all official+unofficial instructions, NMI edge detection, OAM DMA, and open-bus behavior |
| `src/ppu.cpp` + `src/include/ppu.hpp` + `src/palette.inc` | 565 + 123 + 11 | PPU, palette (including 8 precomputed color-emphasis tables, nesRgb) |
| `src/apu.cpp` + `src/include/apu.hpp` | 113 + 21 | APU wrapper: holds global `Nes_Apu apu; Blip_Buffer buf;` |
| `src/cartridge.cpp` + `src/include/cartridge.hpp` | 166 + 30 | iNES loading, mapper factory (switch-based instantiation), PRG/CHR access routing |
| `src/mapper.cpp` + `src/include/mapper.hpp` | 108 + 60 | Mapper base class: PRG/CHR page-table mapping, CHR-RAM, save-state interface |
| `src/joypad.cpp` + `src/include/joypad.hpp` | 58 + 11 | Controller shift register + strobe semantics |
| `src/savestate.cpp` + `src/include/savestate.hpp` | 271 + 62 | Save states (only cstdio/cstring/iostream, no SDL) |
| `src/mappers/*.cpp` + `src/include/mappers/*.hpp` | See §4 | Mapper implementations |
| `lib/Blip_Buffer.cpp`, `lib/Multi_Buffer.cpp`, `lib/Nes_Apu.cpp`, `lib/Nes_Oscs.cpp`, `lib/Nes_Namco.cpp`, `lib/Nes_Vrc6.cpp`, `lib/Nonlinear_Buffer.cpp`, `lib/apu_snapshot.cpp` | 398/201/417/498/160/231/189/124 | blargg audio library (LGPL), pure C++ with no platform dependencies |
| `lib/include/` | Several headers | blargg library headers (including the `boost/` compatibility-header directory) |

The core + mappers + lib total about 6200 lines (src portion), a very small codebase.

### 2.2 SDL2 platform layer (replaced entirely for the X2D port)

| File | Lines | Description |
|---|---|---|
| `src/main.cpp` | 11 | Entry: `GUI::load_settings() → GUI::init() → GUI::run()` |
| `src/gui.cpp` + `src/include/gui.hpp` | 476 + 35 | SDL2 window/rendering/audio queue/event loop/menu assembly; **implements the core callbacks** |
| `src/menu.cpp` + `src/include/menu.hpp` | 316 + 116 | Menu UI (SDL_ttf text, vector/functional), also uses `dirent.h` for directory listing |
| `src/config.cpp` + `src/include/config.hpp` | 164 + 42 | INI settings (SimpleIni submodule) + key mapping; exposes `SDL_Scancode` in the header |
| `lib/Sound_Queue.cpp` + `lib/include/Sound_Queue.h` | 138 + ~60 | blargg's **SDL audio output queue** (SDL_OpenAudio + semaphore-based triple buffering), part of the platform layer, not the core |

### 2.3 Other

- `res/`: font.ttf, init.png, laines_icon.png (menu/window only, can be discarded)
- `SConstruct`, `.gitmodules` (simpleini submodule, uninitialized)

## 3. Core/platform interface coupling points (key to the port)

All core dependencies on the platform converge on **5 functions in the `GUI::` namespace** (declared in `src/include/gui.hpp`, implemented in `src/gui.cpp`). Core code (cpu/ppu/apu/joypad/cartridge/mappers/savestate) performs all I/O through these 5 functions:

```cpp
namespace GUI {
    u8  get_joypad_state(int n);                        // Input polling (core → platform)
    void new_frame(u32* pixels);                        // Video frame output (core → platform)
    void new_samples(const blip_sample_t* samples, size_t count); // Audio output (core → platform)
    bool is_fast_forward();                             // Query fast-forward flag (core → platform)
    // Plus platform-only functions such as init()/run()/toggle_pause()/set_paused() (not called by the core)
}
```

### 3.1 Startup sequence (ordering a custom platform layer must replicate)

```
Cartridge::load(path):
    Read file → parse iNES header (mapperNum = (rom[7]&0xF0)|(rom[6]>>4))
    → new MapperX(rom) (switch factory, see §4)
    → PPU::reset()          // Must precede CPU::power(), because power ticks the PPU
    → APU::reset()
    → CPU::power()          // Clear state + INT<RESET>()
```

`APU::init()` is needed only once per process lifetime (sets up the 96kHz Blip_Buffer, DMC reader,
and IRQ notifier); `Cartridge::load` can be called again on each cartridge change/reset (deletes the old mapper first).

### 3.2 Main loop / frame driving

The platform layer calls this once per frame:

```cpp
CPU::run_frame();   // Execute TOTAL_CYCLES = 29781 CPU cycles ≈ 1 frame
```

- Internally: `remainingCycles += 29781; while (remainingCycles > 0) exec();`
- Each CPU cycle = **3 calls to `PPU::step()`** (`tick_single()` interleaving supports sub-cycle
  precision for open-bus/DMA behavior).
- Audio is finalized at frame end in a fixed order (order-sensitive, handled inside the core; no platform involvement needed):
  `APU::run_frame → Cartridge::run_mapper_audio → Cartridge::end_mapper_audio_frame → APU::end_buffer_frame`.
- Reference pacing: LaiNES's SDL layer uses `SDL_GetTicks()` to cap the loop at 60 FPS; the X2D platform
  can use its own vsync/timer to cap at 60 (1.007...) Hz; the core itself does not depend on wall-clock time.

### 3.3 Obtaining video frames

- Frame buffer: **global `u32 PPU::pixels[256*240]`** (`src/ppu.cpp:18`).
- Delivery timing: the core calls
  `GUI::new_frame(PPU::pixels)` (`src/ppu.cpp:419`) at dot 0 when the scanline enters POST (line 240)—the instant a frame finishes rendering.
- Pixel format: **u32, 0x00RRGGBB layout (alpha byte always 0)**, 256×240.
  SDL uses `SDL_PIXELFORMAT_ARGB8888` + no blending by default, so alpha=0 is harmless.
  **X2D/Android caution**: if the target surface requires alpha (GL RGBA textures with blending enabled, or
  ANativeWindow RGBA), add alpha with `| 0xFF000000` when copying, or the whole frame will be transparent.
- After the new_frame callback returns, the core continues writing to the same buffer (rendering the next frame); the platform should synchronously
  copy it in the callback (about 240KB/frame), or use double buffering.

### 3.4 Obtaining audio

- Format: **mono 16-bit (`blip_sample_t` = short), sample rate 96000 Hz**,
  clock domain 1789773 Hz (`APU::init`: `buf.sample_rate(96000); buf.clock_rate(1789773)`).
- Delivery timing: at frame end in `APU::end_buffer_frame()`, when `buf.samples_avail() >= 4096`,
  read at most `OUT_SIZE = 4096` samples and call
  `GUI::new_samples(outBuf, count)`. Thus **at most one callback and 4096 samples per frame**
  (~42.7ms @ 96kHz; normal output is ~1600 samples/frame, triggered only at >=4096, giving roughly
  one 4096-sample block every 2-3 frames—use actual Blip_Buffer availability and consume count samples in the callback).
- Fast-forward: when `GUI::is_fast_forward()` is true, the core calls `buf.clear()` at frame end to discard the backlog;
  the platform should likewise avoid writing to the audio queue during fast-forward.
- The upstream consumer is `lib/Sound_Queue.cpp` (SDL_OpenAudio 96kHz + 3-buffer semaphore).
  For the X2D port, replace it with an AAudio/Oboe/AAudio-style implementation: a custom lock-free ring buffer, produced by the emu thread
  and consumed by the audio thread. Prefer 96k directly (or change `buf.sample_rate()` and reconfigure—
  that line is in `APU::init`, a core file, so keep 96k if avoiding core changes).

### 3.5 Controller input injection

- The core **polls** button state through `GUI::get_joypad_state(int n)` (n = 0/1), with no callbacks
  or event queue; `src/joypad.cpp` samples and latches on the active $4016 strobe 1→0 transition,
  and reads the A button directly while strobe is high.
- Returns a u8 bitfield (explicit in `src/gui.cpp:269`):
  `bit0=A, bit1=B, bit2=Select, bit3=Start, bit4=Up, bit5=Down, bit6=Left, bit7=Right` (1=pressed).
- X2D integration: map physical camera buttons into a `u8` and return it from the `get_joypad_state`
  implementation; it is called only on the emu thread, so reading an atomic/volatile u8 is sufficient.

### 3.6 Header-level SDL coupling (the only "compile-time" coupling)

- `src/include/gui.hpp` includes `<SDL2/SDL.h>` (also declares `SDL_Texture*`
  and `SDL_Scancode` return values—but only menu.cpp/gui.cpp use those functions).
- `src/include/config.hpp` includes `<SDL2/SDL.h>` and uses `SDL_Scancode`.
- **Solution**: provide a replacement `gui.hpp` for the custom platform layer (put our include path before
  `src/include`), retaining only the 5 prototypes actually called by the core (code block at the start of §3), with no SDL
  include; or, more simply, keep upstream gui.hpp but give the compiler an empty SDL header shim. The former is recommended:
  the core actually references only `GUI::new_frame/new_samples/get_joypad_state/is_fast_forward`.
- Core source files that include `gui.hpp`: `cpu.cpp` (no direct calls), `ppu.cpp`
  (new_frame), `apu.cpp` (new_samples/is_fast_forward), `joypad.cpp`
  (get_joypad_state), `savestate.cpp` (none).

### 3.7 Save states (optional)

`SaveState::save(path)/load(path)` is a pure-core implementation that serializes CPU/PPU/APU/mapper state.
The platform layer need only call it from the menu; no SDL required. Default filenames are derived from the ROM path.

## 4. Supported mappers

The factory is in `src/cartridge.cpp:81-100` (switch):

| mapper | Board/chip | Source files (cpp+hpp lines) | Notes |
|---|---|---|---|
| 0 | NROM | mapper0.hpp (13, header-only) | |
| 1 | MMC1 | 86 + 39 | |
| 2 | UNROM | 42 + 34 | |
| 3 | CNROM | 52 + 36 | |
| 4 | MMC3 | 87 + 64 | IRQ + scanline signals |
| 5 | MMC5 | 762 + 266 | Largest file; directly accesses internals such as `PPU::ciRam/dot/mask/ctrl` (deliberately exposed by ppu.hpp) |
| 7 | AxROM | 50 + 31 | |
| 9 | MMC2 | 104 + 59 | |
| 10 | MMC4 | 102 + 59 | |
| 11 | Color Dreams | 36 + 32 | |
| 24 | VRC6a | 279 + 118 | **Includes VRC6 expansion audio** (lib/Nes_Vrc6) |
| 26 | VRC6b | Reuses Mapper24(rom, true) | Same as above |
| 34 | BNROM/NINA-001 | 66 + 43 | |
| 66 | GxROM | 36 + 32 | |

Unsupported mappers take the default branch: print "mapper N not supported" to stderr and return
(**note: rom memory leaks here and mapper retains its old value—check `Cartridge::loaded()` for load failures**).

## 5. Global singletons / static state (determines platform integration)

LaiNES **has no class-encapsulated Core object**; all emulator state consists of namespace-level globals,
**single-instance and not thread-safe (emu must be driven by one thread)**:

| Namespace | Global state |
|---|---|
| `CPU` (src/cpu.cpp:13-53) | `ram[0x800]`, `A/X/Y/S/PC/P`, `nmi_*`, `irq`, `data_bus`, put/RMW cycle flags, OAM DMA state, `remainingCycles`, `ppu_sub_cycle` |
| `PPU` (src/ppu.cpp:8-30) | `pixels[256*240]` (frame buffer), `ciRam[0x800]`, `cgRam[0x20]`, `oamMem[0x100]`, `oam/secOam`, `vAddr/tAddr/fX`, `ctrl/mask/status`, `scanline/dot`, `nesRgb[8][64]` |
| `APU` (src/apu.cpp:8-12) | `Nes_Apu apu;` (blargg object, contains its own static tables), `Blip_Buffer buf;`, `outBuf[4096]` |
| `Cartridge` (src/cartridge.cpp:24-26) | `Mapper* mapper`, `currentRomPath`, `currentMapperId` |
| `Joypad` (src/joypad.cpp:6-8) | `joypad_bits[2]`, `strobe`, `strobe_was_set_by_put` |
| `GUI` (src/gui.cpp, platform layer) | SDL window/texture/menu pointers, `pause`, `fast_forward` |

Implication: the X2D platform layer is a "global driver"—after init, loop over
`CPU::run_frame()` on its own thread, exchanging data through 4 GUI callbacks; there are no multiple instances and no locks are needed
(the audio callback thread reads only the platform layer's own ring buffer). blargg `Nes_Apu` contains a few
static constant tables, compatible with single-threaded driving from a thread-safety perspective.

## 6. Build considerations (Android NDK / libc++_static)

1. **C++ standard**: upstream SConstruct uses `-std=c++14`. C++17 should compile without issues
   (no relics such as auto_ptr/throw(); older code such as `Nes_Oscs.h` is C++98-style but clean).
   Either C++17 or C++14 works; C++20 is unnecessary.
2. **STL dependencies (very light)**: the core actually uses only the standard headers `<cstdint> <cstring>
   <cstdio> <cstdlib> <iostream> <string>`; `<vector> <functional> <algorithm>
   <csignal> <cctype>` appear only in the platform layer (menu/gui/config). libc++_static covers all of them.
3. **POSIX headers**: `dirent.h unistd.h sys/stat.h` appear only in menu.cpp/config.cpp
   (platform layer, replaced). The pure core has no POSIX dependencies and is highly portable.
4. **Files to exclude/replace**: `main.cpp gui.cpp menu.cpp config.cpp
   lib/Sound_Queue.cpp`; also provide a custom `gui.hpp` (§3.6) and implementations of the 5 `GUI::` functions.
   Compile the rest of src/ + lib/ (except Sound_Queue) unchanged.
5. **Minimal platform layer that works without core changes**:
   `GUI::get_joypad_state` (read button bitmap), `GUI::new_frame` (copy 256×240×4B,
   add alpha 0xFF), `GUI::new_samples` (write ring buffer), `GUI::is_fast_forward`
   (can always return false).
6. **Exceptions/RTTI**: no try/catch or dynamic_cast seen in the core; disabling exceptions on the NDK side
   (-fno-exceptions) requires handling `new` failures (the core uses many raw new calls; normal heap sizes are fine,
   but conservatively, keep exceptions enabled by default).
7. **Performance**: upstream uses `-O3 -march=native`; on NDK, `-O3` + the target ABI (arm64-v8a) is sufficient.
   LaiNES is known for being lean, with ample single-core headroom at 60fps.
8. **Memory**: Mapper construction allocates `new u8[romSize]` (a full ROM copy) + PRG-RAM starting at 8KB +
   CHR-RAM 8KB; one pixel frame is 240KB. Peak memory = ROM size + a few hundred KB, easily handled by the X2D.
9. **LGPL (lib/)**: static linking of blargg requires providing the corresponding object files/
   source access as required by LGPL; if unacceptable, consider replacing the entire APU implementation (substantial work,
   not recommended). LaiNES itself is BSD-2 and only requires retaining copyright notices.
10. **One-time C++ initialization**: `APU::init()` is called in `GUI::init()` (96000Hz);
    the custom platform layer should call `APU::init()` once at startup (or set the sample rate before init—
    the rate is hardcoded in apu.cpp as `buf.sample_rate(96000)`, so without core changes it must remain 96k).

## 7. Appendix: testrom validation record

- `nestest.nes`: 24,592 B = 16(iNES header)+16KB PRG+8KB CHR, header `4E 45 53 1A`,
  PRG=01 CHR=01, flags6/7=0 → mapper 0, horizontal mirroring. Source:
  github.com/christopherpow/nes-test-roms (public test collection from the emulator developer community).
  Purpose: CPU instruction-level correctness (automated mode starts at $C000, compare logs).
- `240pee.nes` (240p Test Suite for NES v0.23, by Damian Yerrick/pinobatch,
  publicly distributed via the official GitHub Release): 65,552 B = 16+64KB PRG+0 CHR(CHR-RAM),
  header `4E 45 53 1A 04 00 21 08` → mapper 2 (UNROM), vertical mirroring, NES 2.0 header.
  LaiNES supports both mappers (0, 2). Both are explicitly publicly distributed, not commercial Nintendo ROMs.
