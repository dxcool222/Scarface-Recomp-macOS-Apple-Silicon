# Game repo split: a thin populous-recomp on top of recomp-kit

Date: 2026-09-13
Status: approved in conversation 2026-09-13; spec pending review
Parent spec: `2026-09-13-recomp-kit-design.md`

## 1. Goal

Let `populous-recomp` (github.com/veritr1x/populous-recomp) build and install
the iOS and macOS ports from its own checkout, without carrying a second copy
of the kit. The game repo keeps only what is Populous's: its `game.toml`,
curated globals, game-specific headers, mods, artwork, smoke scripts, release
notes and docs. The kit is pulled in as a git submodule and driven from thin
wrappers, so `python3 tools/build.py --target ios` in populous-recomp puts
Populous on the iPad.

## 2. Decisions taken in conversation

- Thin game repo plus kit submodule, not a backport and not a mirror.
- The kit keeps a stub game (`games/stub/`) for its own CI and unit tests;
  real games live only in their own repos.
- Populous-only tools and tests move to the game repo now, not in M2.
- The kit stays private for now; the submodule works for the owner only
  until the kit opens up. populous-recomp is not made public by this work.

## 3. Non-goals

- Generalising kit code that still mentions Populous in comments or names
  (`pop_mod_api.h`, `PopRecomp` in tool docstrings, `PopulousRecomp` as the
  CMake project name). M2 does that; `tools/check_game_literals.py` already
  fences the non-comment cases.
- Supporting two games in one build tree, or a `games/` directory with more
  than the stub in the kit.
- Releases from the game repo (`release.yml`, `dist/`). The old workflow is
  removed with the old tree; a kit-based release is later work.
- Preserving the game repo's tracked `translation/`. The kit never tracks
  generated code; a checkout regenerates it with `--regenerate`.

## 4. Repository shapes

### 4.1 populous-recomp after the split

```
populous-recomp/
  kit/                      git submodule -> recomp-kit (pinned commit)
  game.toml                 moved from kit games/populous/
  globals.toml
  core/                     terrain.hpp, entities.hpp, ... (moved)
  tests/                    entity_codec.hpp (moved)
  mods/                     core/, examples/, debug/, smoke/, CMakeLists.txt (moved)
  assets/terrain/           materials-v1.png and its README (moved)
  smoke/                    the Populous smoke scripts (moved from kit tools/recomp/smoke/)
  tools/
    build.py                thin: exec kit/tools/build.py --game-dir <repo root> ...
    test.py                 thin: exec kit/tools/test.py --game-dir <repo root> ...
    setup.py                thin: exec kit/tools/setup.py --game-dir <repo root> ...
    ios_logs.py             thin
    release/README.txt      moved from kit tools/recomp/release/
  docs/
    DISPLAY.md, HD_TEXTURES.md, MODDING.md   Populous-facing docs (from the kit's docs/)
    testing.md              the game-backed suites, rewritten for the wrappers
  original/                 ignored: the developer's GOG installation
  analysis/                 ignored: Ghidra listings
  build/                    ignored: translation, texture pack, apps, logs
  README.md, CHANGELOG.md, CONTRIBUTING.md, AGENTS.md, LICENSE, NOTICE
  .gitmodules, .gitignore, .github/workflows/checks.yml
```

`src/`, `third_party/`, `cmake/`, `translation/`, the old `CMakeLists.txt`,
`CMakePresets.json`, `Makefile`, `requirements-dev.txt` and
`release.yml` are removed. The head before the change is tagged
`legacy-macos-source`.

### 4.2 recomp-kit after the split

```
recomp-kit/
  games/stub/               game.toml, globals.toml: a fake game for link-only builds
  (everything else as today, minus games/populous, tools/recomp/smoke/*.script
   that are Populous's, tools/recomp/release/, docs/DISPLAY.md, HD_TEXTURES.md, MODDING.md)
```

The `original` and `analysis` symlinks at the kit root go away; nothing in the
kit resolves them any more.

## 5. The kit's external game directory

### 5.1 CMake

- `RECOMP_GAME_DIR` (cache PATH) replaces `RECOMP_GAME`. Default
  `${POP_ROOT}/games/stub`. It must hold `game.toml`.
- `POP_BUILD_ROOT` (cache PATH) replaces the hard-wired `${POP_ROOT}/build`.
  Default `${POP_ROOT}/build`. `POP_BUILD_DIR` derives from it exactly as
  today (`/stub`, `/ios`, `/ios-stub` suffixes), and so do `POP_GEN_DIR`
  (`${POP_BUILD_ROOT}/recomp/gen`), the texture pack, the symbol table the
  iOS bundle copies, and `tools/recomp/finish_bundle.py`'s inputs.
- `add_subdirectory(${RECOMP_GAME_DIR}/mods ${CMAKE_BINARY_DIR}/game-mods)`
  when that directory has a `CMakeLists.txt` and the build is not iOS;
  the game's mods file receives `RECOMP_GAME_DIR` and `POP_OUT` like today.
- The roots tests (`roots_probe`, `roots_tests.py`) move into the game's
  `mods/CMakeLists.txt`, where their sources are.
- `cmake/IosBundle.cmake` and `MacBundle.cmake` read the developer game
  directory from the generated cmake fragment, which now renders absolute
  paths (section 5.3).
- CMake presets keep `${sourceDir}/build/cmake/<preset>` as `binaryDir`, so
  `cmake --preset macos` inside the kit still works alone. `tools/build.py`
  passes `-B ${POP_BUILD_ROOT}/cmake/<preset>` when a game directory outside
  the kit is used, so the CMake tree lands beside the game's other outputs.

### 5.2 tools/build.py, tools/test.py, tools/setup.py, tools/ios_logs.py

- `--game-dir PATH` replaces `--game NAME`. Default `games/stub` relative to
  the kit. The build root is `<game-dir>/build` when the game directory is
  outside the kit, else the kit's `build/`; `--build-root` overrides.
- `buildlock.BuildLock` takes the build root, not the kit root, so a build in
  the game repo locks the game repo's `build/recomp/.lock`.
- `publish_generated`, `run_translator`, `texture_pack`, `archive_path`,
  `ios_app_bundle` and `test.py`'s `mods()`/`gameplay()` use the build root.
- `test.py --gameplay` reads its script from `<game-dir>/smoke/native-options.script`;
  the mode probe and other generic tools keep living in the kit.
- `setup.py` writes into `<game-dir>/original` and `<game-dir>/analysis`.

### 5.3 game.toml path resolution

`developer_exe` and `translate.listings` resolve relative to the directory
holding `game.toml`. `tools/game_config.py` returns them resolved
(`cfg["developer_exe_path"]`, `cfg["listings_path"]`, absolute `Path`s) and
`tools/gen_game_config.py` renders `RECOMP_DEVELOPER_GAME_DIR` and
`RECOMP_DEVELOPER_EXE` as absolute paths. Populous's `game.toml` keeps
`developer_exe = "original/gog/D3DPopTB.exe"` and
`listings = "analysis/decompiled/D3DPopTB.exe"`, which now name
`populous-recomp/original/...` and `populous-recomp/analysis/...`.

### 5.4 The stub game

`games/stub/game.toml`:

```toml
[game]
id = "stub"
name = "Stub Game"
app_name = "StubRecomp"
bundle_id = "dev.recompkit.stub"
executable = "STUB.EXE"
sha256 = "0000000000000000000000000000000000000000000000000000000000000000"
image_base = 0x00400000
entry_point = 0x00401000
guest_root = 'C:\Stub'
developer_exe = "original/STUB.EXE"

[translate]
listings = "analysis/STUB.EXE"
globals = "globals.toml"
animation_counter = 0x00500000
volatile_reads = []

[hooks]
frame_clock_begin = 0x00401100
frame_clock_wait = 0x00401200
frame_clock_wait_clamp = 0x00401300
frame_clock_clamp_deadline = 0x00600000
frame_clock_wait_deadline = 0x00600010
cursor_surface_ptrs = [0x00600100, 0x00600104]
mouse_vtable = 0x00600200
mouse_device_ptr = 0x00600300
mouse_device_right = 0x00600340
camera = 0x00600400

[bundle]
exclude = []
```

`games/stub/globals.toml` declares every global the kit's code reads
(`grep -rho 'RECOMP_GLOBAL_[A-Z_]*' runtime dx host mods | sort -u` is the
list) with distinct addresses in the 0x00700000 range and the sizes, strides
and counts the code expects. The stub exists so every `RECOMP_*` macro has a
value; no test asserts anything about its numbers.

## 6. What moves, what is re-pointed

| Kit file | Fate |
|---|---|
| `games/populous/**` | moves to the game repo root (section 4.1) |
| `tools/recomp/smoke/*.script` (every one names Populous levels, modes or addresses) | move to `populous-recomp/smoke/`; `tools/test.py --gameplay` and `host/tests/integration_tests.sh` read them from `<game-dir>/smoke/` |
| `tools/recomp/release/README.txt` | moves to `populous-recomp/tools/release/` |
| `docs/DISPLAY.md`, `docs/HD_TEXTURES.md`, `docs/MODDING.md` | move to `populous-recomp/docs/`; the kit's `docs/architecture.md`, `code-guide.md`, `testing.md` stay and lose their Populous-only paragraphs |
| `tools/recomp/oracle.py`, `tools/recomp/translate.py`, `tools/recomp/build_core.py` | `--game-dir` required (or `RECOMP_GAME_DIR` in the environment); the Populous defaults go |
| `tools/recomp/tests/test_translate.py` (runs the real binary under Unicorn) | stays in the kit; skips with a clear message unless `RECOMP_GAME_DIR` names a game whose `developer_exe` exists; run from the game repo's `tools/test.py --translate` |
| `tools/recomp/tests/test_translate_config.py`, `tests/test_game_config.py`, `tests/test_stage_game_files.py`, `tests/test_build_py.py` | re-pointed at `games/stub`; the Populous-specific assertions (hook values, exclusions) move to `populous-recomp/tests/test_game_config.py` |
| `runtime/tests/runtime_tests.cpp` (label `game`) | stays; the literal `original/gog/D3DPopTB.exe` becomes `RECOMP_DEVELOPER_EXE`; the test's working directory becomes `RECOMP_GAME_DIR` |
| `mods/tests/game_view_tests.cpp` (`games/populous/tests/entity_codec.hpp`), `mods/tests/present_events_tests.mm` (luawalk example), `mods/tests/fixtures/display_projection.c` (core display mod) | these test kit code against Populous fixtures: they compile only when `RECOMP_GAME_DIR` provides the files (`if(EXISTS ...)`), with the include path `${RECOMP_GAME_DIR}`; they are labelled `game` |
| `host/tests/integration_tests.sh` (game-backed; runs the roots tests, plugins and smoke scripts) | takes the game directory from `RECOMP_GAME_DIR` in its environment (set by `tools/test.py --integration`, a new mode that replaces calling the script by hand); `host/smoke_main.cpp`'s comment names `<game>/tests/entity_codec.hpp` |
| `host/CMakeLists.txt` `mods` label tests (`present_events_tests`) | as above |
| `tools/check_game_literals.py` | unchanged tokens; now also runs over `games/stub` to prove the stub carries no Populous value |

The rule: a file whose content only holds for Populous moves; a file that
exercises kit code and merely uses Populous as its fixture stays, guarded by
the presence of the external game directory, and is labelled `game`.

## 7. Thin wrappers in the game repo

`populous-recomp/tools/build.py`:

```python
#!/usr/bin/env python3
"""Build Populous with the kit in kit/: every option is the kit's."""
import os, subprocess, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
KIT = ROOT / "kit"
if not (KIT / "tools/build.py").is_file():
    sys.exit("The kit submodule is missing: run `git submodule update --init`")
sys.exit(subprocess.call([sys.executable, str(KIT / "tools/build.py"), "--game-dir", str(ROOT)] + sys.argv[1:],
                         cwd=ROOT))
```

`test.py`, `setup.py` and `ios_logs.py` are the same four lines with the
kit script's name. The Python environment is the game repo's `.venv`,
created from `kit/requirements-dev.txt` (the game repo's `CONTRIBUTING.md`
says so; it has no requirements file of its own).

## 8. CI

- Kit `checks.yml`: unchanged in shape; every job now configures with the
  default `games/stub`. The `tooling` job's `tools/test.py` runs the
  portable suites against the stub. No job needs Populous.
- Game repo `checks.yml`: one job on `macos-15` and one on `ubuntu-24.04`:
  `actions/checkout` with `submodules: recursive`, a venv from
  `kit/requirements-dev.txt`, then `python3 tools/build.py --stub`
  (link-only configure of this game's config through the kit, macOS also
  `--target ios --stub`), `python3 tools/test.py` (the kit's portable suites
  plus `tests/test_game_config.py` here), `python3 kit/tools/check_game_literals.py`
  is not run (the game repo may name its game). The submodule checkout of a
  private kit needs a deploy key or PAT secret (`KIT_TOKEN`) in the game
  repo; until it exists the game repo's CI is allowed to fail on checkout
  and the workflow says so in a comment.

## 9. Migration order

1. Kit, branch `game-repo-split`: `games/stub`, `RECOMP_GAME_DIR` /
   `POP_BUILD_ROOT`, `--game-dir`, path resolution, test re-pointing, tool
   defaults. Verified by building Populous from the still-present
   `games/populous` through the new path (`--game-dir games/populous`) on
   macOS and iOS, running `tools/test.py`, `--native`, `--mods` and
   `--gameplay`, and by the stub build passing with `--game-dir games/stub`.
2. Kit: move the Populous-only files into a staging copy for step 3, delete
   them from the kit, fix references, CI green. Tag `m1.1`.
3. Game repo, branch `thin-kit`: tag `legacy-macos-source`, replace the tree
   (section 4.1), add the submodule at `m1.1`, wrappers, docs, CI. Verify
   `python3 tools/build.py --regenerate`, `--target ios`, `tools/test.py`,
   `--native`, `--mods`, `--gameplay` from the game repo; play on the iPad.
   Tag `kit-1`.

Between steps 1 and 3 the developer's iPad build comes from the kit as
today; after step 3 it comes from populous-recomp.

## 10. Error handling

- A `--game-dir` without `game.toml`, or a relative one, is rejected by
  `tools/build.py` with the path it looked at.
- A game directory inside the kit other than `games/stub` is rejected once
  step 2 lands (the kit holds no real game).
- `cmake --preset ios` from the game repo without the submodule initialised
  fails in the wrapper with the `git submodule update --init` hint.
- A build root on a different volume from the kit is fine: nothing assumes
  the two share a parent.
- The old game repo layout in someone's checkout: `git pull` replaces the
  tree; their `original/`, `analysis/` and `build/` are ignored and survive,
  though `build/` must be regenerated (`--regenerate`) because the kit's
  layout of `build/recomp` matches the old one only by accident.

## 11. Testing

- Kit: `tools/test.py` (portable, on the stub), `tools/test.py --native`
  and `--mods`/`--gameplay` with `--game-dir games/populous` before step 2
  and with `--game-dir ../populous-recomp` after step 3; CI on the stub.
- New unit tests: `tests/test_build_py.py` covers `--game-dir` validation and
  the build-root choice; `tests/test_game_config.py` covers absolute path
  resolution; `tests/test_game_literals.py` covers the stub.
- Game repo: the thin wrappers are exercised end to end by the iPad build;
  `tests/test_game_config.py` holds the moved Populous assertions.
