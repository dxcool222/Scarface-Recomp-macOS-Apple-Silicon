# Testing

Run checks appropriate to your change. Every suite's output belongs under ignored
`build/`; requested tests must report failure rather than silently skip prerequisites.

| Command | What it checks | Needs game files? |
| --- | --- | --- |
| `tools/test.py` | Setup failures, synthetic texture processing and display-mode tooling | No |
| `tools/format.py` | Consistent formatting of handwritten native code | No |
| `tools/test.py --compile-only` | Every native test binary this platform has compiles (macOS, Linux, Windows) | No |
| `tools/test.py --native` | Portable suites everywhere; runtime, adapters, offscreen Metal and UI tests on macOS | Partly: `game`-labeled suites need the image |
| `tools/test.py --mods` | Real loader/hooks/settings/native Options and replay contracts | Yes, plus translated archive |
| `tools/test.py --gameplay` | Menu navigation, mode cycling, selection, movement and clean exit | Yes, plus translated archive |

Native suites are CTest entries with labels: `nogame` runs everywhere and in CI,
`game` needs your installation, `gpu` needs a Metal device, `device` needs a
real Metal and audio device (the offline audio render is not what a hosted CI
runner produces), `mods` needs the translated archive and the entity snapshot `tools/test.py --mods` captures. Run
one directly with `.venv/bin/ctest --test-dir build/cmake/macos -L nogame` or `-R dx_tests`.
Two suites cover the on-screen controls, both `nogame`: `controls_tests` for the
layout model, geometry, routing, virtual pad, mapped binding, raster primitives
and editor (including the old keypad geometry as a regression oracle), and
`pad_tests` for the DirectInput joystick and the three XInput DLLs, which lives
beside `dx_tests` because it defines the `host_pad_*` callbacks strongly.
Every `tools/test.py` mode takes `--game-dir /abs/path/to/<game>`; without it
the kit's stub game is used and the `game`-labelled suites report a skip.

Invoke these with `.venv/bin/python`. The native tests need a macOS Metal device;
CI compiles them but does not claim GPU or original-game execution. The mod suite
first builds and runs a deterministic 32-frame entity capture from your own game
in an isolated directory; no saved capture from another checkout is needed. The gameplay
runner uses an isolated profile and original textures unless a local pack exists.

For a Windows cross-build, set `LLVM_MINGW_ROOT`, then run
`tools/build.py --preset windows-cross --stub --target app` and
`tools/test.py --preset windows-cross-stub --compile-only`. CI verifies the
Indeo 5/AVI and Vorbis/Ogg components and uses Wine to load `dx_tests.exe` with
only its three copied FFmpeg DLLs. `RECOMP_TEST_AUDIO=/path/to/track.ogg` exercises
the CD-music decoder and requires nonzero PCM; CI generates its own sine tone.
`RECOMP_TEST_AVI=/path/to/movie.avi` exercises the guest AVIFile/Indeo imports,
requiring changing nonblack frames. Movie probes use private local inputs,
never uploaded. These headless probes do not open an audio device or game window.

For instruction-translation changes, use the original differential harness:

```sh
.venv/bin/python tools/recomp/tests/test_translate.py --help
.venv/bin/python tools/recomp/tests/test_translate.py
.venv/bin/python -m pytest tools/recomp/tests/test_translate_hooks.py
```

Three translator suites need no game and run in `tools/test.py`:
`test_translate_insns.py` runs synthetic listings of individual instruction
forms through the translator, compiles them with the test harness and
compares registers, flags and memory with Unicorn; `test_jumptables.py`
decodes the jump-table shapes on synthetic functions over a fake image;
`test_translate_driver.py` covers driver rules such as what a withdrawn block
leaves behind. Add a case there first when the translator meets an
instruction or table shape it does not handle.

The differential harness compares translated routines with original instructions
under Unicorn. Unicorn is a development tool, not part of the playable app.

## Manual gameplay checks

- Start a level, select a person, issue a move order and observe the destination.
- Compare Classic/Enhanced and Wide on/off in a window wider than an 800×600 canvas.
- Cycle resolution through 640×480, 800×600 and 3840×2160, then resume play.
- Change graphics/host settings, quit normally, relaunch and check persistence.
- Test Command-Q, focus loss/return and mouse motion at all four edges in each window mode.
- Play the intro long enough to expose streaming stalls; check music and effects in a level.
- Compare animation speed at 60 and 120 FPS. Capture frame-pacing data during real play.

Report macOS/device, selected resolution, window mode, rendering mode and frame
limit. State whether FPS counts submitted, completed or displayed frames. A pinned
smoke clock is deterministic test timing and cannot substantiate real-time performance.

## Current local evidence

The source snapshot includes fixes exercised through native Options, live
640×480 → 800×600 → 4K → 640×480 transitions, unit movement and clean exit.
The initial publication additionally runs the source-only CI checks and local
native suites built through CMake from the standalone checkout, and the Linux and
Windows portable-layer suites in CI. Long campaign completion, multiplayer
and sustained 4K120 remain unverified; publish measurements with their conditions.
