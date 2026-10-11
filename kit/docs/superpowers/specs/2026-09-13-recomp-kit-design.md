# recomp-kit design

> Amended 2026-09-13 by `2026-09-13-game-repo-split-design.md`: games live in their own
> repositories with the kit as a submodule; `games/` in the kit holds only the stub game.

Date: 2026-09-13
Status: approved 2026-09-13

## 1. Goal

A reusable pipeline that statically recompiles 32-bit x86 Windows games into
native applications for macOS, iOS, Android, Linux and Windows, with no JIT
and no emulator at run time. The kit is a port factory: a supported game is one
command for its owner, and an unsupported DirectDraw-era game costs an
experienced person days rather than months.

The kit consolidates three existing workspaces:

- `populous-recomp-checkout`: the translator, runtime, Win32 and DirectX shims,
  GPU backends, SDL3 host and verification oracle. This is the base.
- `lemon`: cold-image capture for packed executables and the trace of what an
  iOS bring-up needs.
- `proton-metal` and `wine-fex-ios`: research that rules out interpreter and
  JIT routes on iOS. Nothing is reused; the conclusions justify this design.

## 2. Non-goals

- A universal player app. iOS forbids loading unsigned native code, so every
  game is its own app, built and signed by its owner.
- Drop-in compatibility for arbitrary games. Static recompilation is per-game
  work; the kit makes that work cheap and repeatable.
- Shader-model Direct3D (8 and 9 with vertex or pixel shaders), 16-bit or DOS
  games, packed or DRM-protected executables, self-modifying code, and
  multiplayer networking. These are out of scope for the first version.
- Shipping any game code or assets. The repository holds tooling, shims and
  per-game configs made of hashes, addresses, stub lists and input maps.

## 3. Users and workflows

### 3.1 Owner of a supported game

Requires a machine with the kit's toolchain and their own DRM-free installer.
iOS additionally requires macOS, Xcode and an Apple ID.

```
recomp build majesty --installer ~/Downloads/setup_majesty_gold_hd.exe --target ios
```

The command extracts the installer, verifies the executable hash against
`games/majesty/game.toml`, translates, generates the platform project, builds,
signs and installs. Assets are copied into the app's data directory, never into
the binary. Saves live in the app's data directory and are visible through the
platform's file browser.

### 3.2 Porter bringing a new game

```
recomp analyze --installer setup_game.exe
```

Reports the executables and DLLs to lift, compiler and packing status, the
import surface split into implemented, auto-stubbed and unsupported, the
DirectX interfaces requested, and a verdict on whether the game is inside the
supported envelope. The porter then builds for the desktop host, plays, and
reads the run report of trapped addresses and stub calls. Rebuilding folds new
addresses in. When the game plays, the config directory is a pull request.

### 3.3 Iteration loop

```
analyze -> translate -> build -> run -> report -> (edit config) -> translate ...
```

Every stage is idempotent and cached by input hash, so a config edit reruns
only translation and compilation of affected chunks.

## 4. Architecture

```
recomp-kit/
  cli/        the `recomp` command; orchestrates everything below
  lift/       ghidra headless export, translator, coverage and import reports
  runtime/    cpu semantics, guest memory, multi-module PE loader, scheduler,
              unlifted-address trap, run reporting
  win32/      kernel32 user32 gdi32 winmm advapi32 ole32 shell32 version
              ws2_32 shims, plus the auto-stub generator
  dx/         ddraw, d3d up to 7, dsound, dinput; d3d8/9 fixed function later
  host/       SDL3 platform layer, GPU seam with metal, vulkan and fake
              backends, audio mixer, input mapping, packaging per target
  platform/   os.h seam with POSIX and Win32 implementations; runtime/ links it
  verify/     unicorn oracle, snapshot and differential tests, smoke runner
  games/      one directory per game: game.toml, symbols, stubs, input map,
              patches; no game bytes
  docs/
```

M0 note: `win32/` and the `lift/`/`verify/` split of the Python tooling are
deferred to M2, when the auto-stub generator and the `recomp` CLI are written.
`mods/` is carried as an opaque component because every host links it.
`tools/setup.py`, the smoke scripts, `tools/recomp/package.py` and the
texture-pack tooling remain Populous-specific until M2 and M4 respectively.

Rule: nothing above `host/` includes a platform header, nothing in `runtime/`,
`win32/` or `dx/` knows a specific game, and generated code depends only on
`runtime/`.

### 4.1 cli

A Python entry point with subcommands `analyze`, `translate`, `build`, `run`,
`report`, `verify`, `package`. Reads `games/<name>/game.toml`, resolves the
toolchain, and calls the stage modules. Every subcommand prints a structured
summary and writes a JSON twin next to it for scripting.

### 4.2 lift

- `ghidra/`: headless export scripts derived from `tools/decompile.sh`,
  `ExportProgram.java` and `ImportAnnotations.java`. Output is a per-module
  listing directory: `functions.tsv`, per-function `.asm`, jump tables, and
  data cross-references.
- `translate/`: the x86-to-C translator, seeded from
  `tools/recomp/translate.py` (3,183 lines). Changes required for generality:
  - move the 17 hard-coded `VISUAL_ANIMATION_READS` addresses and every other
    Populous literal into the game config as `[translate.volatile_reads]`;
  - accept a list of modules, not one executable, and emit one output
    directory per module with a shared symbol namespace keyed by
    `module:rva`;
  - emit a per-module coverage report: translated, skipped, unsupported
    mnemonics, and unresolved indirect targets;
  - accept an `extra_entry_points` list produced by run reports.
- `imports/`: reads each module's import table, matches against the shim
  registry, and emits `imports.json` with three buckets: implemented,
  auto-stub, unsupported. The auto-stub generator emits a C stub per entry that
  logs the call, returns a configurable default, and counts calls for the run
  report.
- `unpack/`: the cold-image capture path from `lemon/tools/capture_cold_image.c`
  for executables that unpack themselves at start. Optional stage, off by
  default, produces a memory image that replaces the on-disk sections.

### 4.3 runtime

- `cpu`: register file, flags, x87 and SSE state, seeded from
  `tools/recomp/runtime/x86.h`. x87 precision is implemented explicitly with
  a fixed 64-bit mantissa model and a control-word aware rounding helper;
  host `long double` is never used, because it differs between x86 Linux and
  every ARM target.
- `memory`: a single 32-bit guest arena reserved through the platform layer.
  Guest addresses index the arena and are never host pointers. Hosts are 64-bit
  only.
- `loader`: maps every configured module into the arena at its preferred base
  or a relocated one, resolves imports between lifted modules first, then
  shims, then auto-stubs. Seeded from `src/recomp/runtime/loader.cpp`.
- `scheduler`: replaces the single-baton cooperative scheduler with
  deterministic green threads. Each guest thread gets its own host stack via
  the platform layer; switches happen only at shim boundaries and at explicit
  yield points, so runs stay reproducible for the oracle. Preemption is
  simulated by a tick counter at yield points, never by signals.
- `trap`: every indirect call and jump goes through a table lookup. A miss
  raises an unlifted-address trap that records the address, the call stack of
  guest return addresses, and the module, then aborts the frame with a clear
  message. In report mode the trap logs and returns a configurable value so
  a session can collect many misses before stopping.
- `report`: writes `run-report.json` on exit or crash with trapped addresses,
  stub call counts, unsupported DirectX calls, and frame timing.

### 4.4 win32

Seeded from `src/recomp/runtime/kernel32.cpp` (3,881 lines) and
`user32.cpp` (1,054 lines), split per DLL. Rules that apply to all shims:

- The guest filesystem is case-insensitive and uses backslashes. Every path
  is normalized against a case-folded index of the game directory before it
  reaches the host, so Linux and Android behave like Windows.
- Registry, environment and locale calls answer from a per-game in-memory
  store that persists to a JSON file in the data directory.
- Time sources are monotonic and driven by the host frame clock so the oracle
  can replay them.
- Any shim that cannot honor a call returns the documented failure code and
  records the call in the run report. Shims never abort.

New shims for the first two games beyond Populous: winmm (timers, wave out,
MCI music), ws2_32 (fails cleanly, no networking), dplayx (reports no
sessions), version, advapi32 registry, ole32 CoInitialize family. Third-party
DLL exports such as Bink and GOG Galaxy are handled by the stub generator with
per-game defaults, so cinematics skip and Galaxy reports offline.

### 4.5 dx

Seeded from `src/recomp/dx/` (about 23.5k lines), which already models
IDirectDraw through version 4, IDirectDrawSurface through 4, palettes and
clippers, IDirect3D and IDirect3D2 with device, viewport, material, light and
texture, DirectSound, DirectInput and QMixer. Draws are recorded, never
rasterized, and leave through `host_api.h` callbacks.

Additions, in order: IDirectDraw7 and IDirectDrawSurface7, Direct3D 3 through 7
fixed function, then Direct3D 8 and 9 fixed function as a separate milestone.
Shader-model support is out of scope.

### 4.6 host

- `platform/`: the `os.h` seam from `src/recomp/platform/os.h` with threads,
  virtual memory, plugins, files and time. SDL3 backs window, events, touch,
  mouse, keyboard, gamepad and audio output on every target. The existing
  `host/sdl/main.cpp` and `host/audio/sdl_sink.cpp` are the starting point.
- `gpu/`: the `gpu.h` seam with the metal, vulkan and fake backends kept as
  they are. Metal on macOS and iOS, Vulkan on Android, Linux and Windows. The
  fake backend runs headless tests on all five.
- `input/`: a per-game input map that binds touch gestures and gamepad to
  the guest's mouse and keyboard. Right-click, drag and modifier chords are
  first-class because RTS interfaces depend on them.
- `package/`: one packager per target. macOS `.app`, iOS `.app` via a generated
  Xcode project and the owner's signing identity, Android APK via Gradle and
  the NDK, Linux tarball, Windows folder. Assets are staged into the data
  directory on first launch from a user-chosen location, never bundled.

### 4.7 verify

- `oracle/`: the Unicorn differential from `tools/recomp/oracle.py` (718
  lines), generalized to any module set. Compares registers, flags and dirty
  memory per call and return event against the recompiled binary for N frames.
- `snapshot/`: golden frame and audio snapshots per game per backend, compared
  with a perceptual threshold.
- `smoke/`: boots each game in `games/` headless on every CI platform and
  checks that the run report is clean for the first N frames.

### 4.8 games

```
games/majesty/
  game.toml          name, installer patterns, executable sha256, modules,
                     entry point, data layout, translate options
  symbols.json       function names and globals, mostly from Ghidra, curated
  stubs.toml         per-import default returns and skip lists
  input.toml         touch and gamepad map
  patches.toml       guest byte or call patches with a reason each
  README.md          status, known issues, how far it has been played
```

`game.toml` schema, first version:

```toml
[game]
name = "Majesty Gold"
executable = "MajestyHD - Old.exe"
sha256 = "0000000000000000000000000000000000000000000000000000000000000000"  # example value; analyze fills it in
modules = ["MajestyHD - Old.exe"]
data_dirs = ["Data", "Quests", "Music"]

[translate]
volatile_reads = []
extra_entry_points = []

[host]
resolution = "native"
aspect = "keep"
```

## 5. Data flow

1. `analyze` extracts the installer, identifies modules, hashes them, runs
   Ghidra headless per module, and emits listings plus `imports.json` and
   `analysis-report.json`.
2. `translate` emits C per module into `build/<game>/gen/`, plus the dispatch
   table, coverage and the auto-stubs.
3. `build` configures CMake for the target, compiles generated code and the
   kit, links the host, and packages.
4. `run` launches the artifact with report mode on. On exit the run report is
   written to the data directory.
5. `report` reads run reports, proposes config edits (new entry points, stub
   defaults that were hit) and prints them as a diff against `games/<name>/`.

## 6. Cross-platform rules

- Hosts are 64-bit. Guest memory is an arena, so host page size (16 KB on iOS)
  is irrelevant to guest layout.
- No host `long double`. No host locale-dependent formatting in shims.
- Generated code compiles with clang everywhere; Windows uses clang-cl.
  Generated chunks are compiled at `-O1` by default and cached per chunk hash.
- Only the iOS packager requires macOS. Analysis, translation, verification
  and the other packagers run on any host with Java, Python, CMake and the
  target SDK.
- CI builds all five targets and runs headless smoke on macOS, Linux and
  Windows runners from the first commit. iOS and Android are compile-only in
  CI and device-tested by hand until simulator runs are stable.

## 7. Error handling

| Condition | Behavior |
|---|---|
| Executable hash mismatch | `analyze` and `build` stop with the expected and actual hashes and the config that claimed them |
| Unsupported mnemonic | translator emits a trap stub, lists it in coverage, and the build proceeds |
| Unlifted address hit at run time | trap records it; strict mode aborts the frame, report mode continues with the configured return |
| Import not implemented | auto-stub logs and returns the configured default; unsupported bucket fails `analyze` unless overridden |
| DirectX call outside modeled surface | shim returns `E_NOTIMPL`, records the interface and method in the run report |
| Oracle divergence | `verify` prints the first diverging event with guest address, register diff and dirty memory range |

## 8. Testing

- Unit tests per component: cpu semantics against Unicorn for every supported
  mnemonic, loader against synthetic PEs, scheduler determinism, path
  normalization, each shim's documented failure codes.
- Headless integration on the fake GPU backend for every game in `games/`.
- Oracle parity for Populous is the regression gate for translator changes:
  byte-identical memory through 32 frames, as it is today.
- Device smoke on one iPad and one Android tablet per milestone.

## 9. Milestones

### M0: consolidate

Move the generic parts of `populous-recomp-checkout` into this repository,
parameterize the Populous literals, and build Populous from `games/populous/`.
Acceptance: the macOS app plays as before, oracle parity holds, and CI compiles
Linux and Windows.

### M1: iOS

Add the iOS packager, SDL3 iOS lifecycle, Metal on iOS, touch map. Acceptance:
Populous menu, level entry and unit control on a stock iPad, signed with a
free Apple ID, at the game's native frame rate.

### M2: generality tooling

Multi-module lifting, import-surface report, auto-stub generator, unlifted
trap and run reports, green-thread scheduler, case-insensitive filesystem.
Acceptance: `analyze` runs on Majesty's DirectDraw build and on Lemonade
Tycoon and produces correct reports; Populous still passes.

### M3: second and third games

Bring up Majesty Gold's DirectDraw build with winmm, dplayx and Bink stubs,
and Lemonade Tycoon through the cold-image path, on macOS and iOS.
Acceptance: first quest of Majesty and one full day of Lemonade playable on
both.

### M4: Android and desktop

Android packager and device test, Linux and Windows packagers with a played
smoke on each. Acceptance: Populous on an Android tablet, all three games
booting on Linux and Windows CI.

### M5: Direct3D 8 and 9 fixed function

Extend `dx/` and validate with Majesty HD, whose MSVC runtime DLLs also
exercise multi-module lifting under load.

## 10. Risks

- Function discovery misses code reached only through vtables; mitigated by
  the trap and report loop, and by trace-driven entry points from the oracle.
- Green threads break a game that expects true preemption; mitigated by
  yield points inside every blocking shim and a per-game tick budget.
- Generated code size slows every target's build; mitigated by chunk caching
  and `-O1` defaults.
- Free Apple ID signing expires weekly; documented, with SideStore as the
  refresh path.
- The long tail: games that boot break late in their content; mitigated only
  by play testing and the report loop, and stated honestly in each game's
  README.

## 11. Licensing

The kit ships under a permissive license. Generated code is derived from the
owner's game binary and stays on their machine. Unicorn is GPL-2 and is used
only in `verify/`, never linked into shipped apps. Third-party components
carried over from the Populous work keep their notices.

## 12. Decisions taken

- SDL3 is the single host layer on all targets. Lemon's UIKit shell is not
  carried over.
- Metal and Vulkan stay as two backends behind `gpu.h`; no MoltenVK.
- Per-game applications, self-signed by the owner, on every platform.
- Populous on iOS is proven before any new game is lifted.
- Majesty's first target is the DirectDraw-only build, not the HD build.

## 13. Decisions locked on 2026-09-13

- Repository name is `recomp-kit`. It stays private until the consolidation
  milestone and the iOS proof are done; per-game configs are contributed
  by pull request once it goes public.
- The Lua mod runtime from `pop-metal` stays out of the kit until a second
  game needs it. Populous builds without mods in the kit until then.
- Minimum platforms: iOS 17, Android 10 with Vulkan 1.1, macOS 14, and the
  current Linux and Windows releases that SDL3 supports.
