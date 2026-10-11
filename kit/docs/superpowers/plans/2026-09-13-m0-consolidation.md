# M0 Consolidation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the generic parts of the Populous recompilation into this repository, put every Populous-specific value behind a per-game config directory, and build Populous from `games/populous/` with no regression.

**Architecture:** The checkout at `<workspace>/populous-recomp-checkout` (commit `b450dfa8bae7fe568192ef61028ed82489cba394`) is imported verbatim minus its tracked translation, then its C/C++ tree is promoted to the spec's top-level layout. A generator turns `games/<id>/game.toml` plus `globals.toml` into a `game_config.h` header and a `game_config.cmake` fragment at configure time; runtime, shims, hosts and the translator read identity, addresses and paths from there instead of literals. A stub translation lets CI link every host without game code.

**Tech Stack:** C11 and C++20 under clang, CMake 3.24 with Ninja, SDL3 3.4.16 fetched by CMake, Python 3.9 tooling (`tomli` for TOML), Ghidra listings as translator input, Unicorn for the differential oracle, pytest for Python tests, CTest for native tests.

**Spec:** `docs/superpowers/specs/2026-09-13-recomp-kit-design.md`

## Global Constraints

- Nothing under `runtime/`, `dx/`, `host/` or `platform/` may contain a Populous literal in code (comments are fine); `tools/check_game_literals.py` enforces this from Task 4 on.
- No game bytes are tracked: no `translation/`, `original/`, `analysis/`, `build/`, generated C, or game assets. `tools/check_repo.py` and `.gitignore` already block most of this.
- Platform calls go through `platform/os.h`; no `#ifdef` on the platform outside `os_posix.cpp` and `os_win32.cpp`.
- Native code builds only through `tools/build.py` and `tools/test.py`; never invoke a compiler directly except where a step says so for a syntax check.
- Handwritten C, C++ and Objective-C++ is formatted with `.venv/bin/python tools/format.py --write` before every commit.
- Python floor is 3.9 (the Mac ships 3.9.6), so TOML is read through `tomllib` with a `tomli` fallback.
- Guest addresses stay 32-bit guest addresses; never host pointers.
- Every commit leaves `tools/test.py` (portable suites) passing. Tasks that touch native code also leave `tools/test.py --native` passing.
- Paths in this plan: `KIT=<workspace>/recomp-kit`, `SRC=<workspace>/populous-recomp-checkout`.

## Deviations from the spec recorded by this plan

- `platform/` is a top-level directory beside `runtime/`, because `runtime/` links it. The spec's layout listing gains that line in Task 8.
- `win32/` is not split out in M0: `kernel32.cpp`, `user32.cpp` and `misc.cpp` stay in `runtime/` as one CMake target. The split happens in M2 with the auto-stub generator.
- The Python tooling stays under `tools/` in M0; the `lift/` and `verify/` split happens in M2 with the `recomp` CLI.
- The mod foundation (`mods/`) is imported as an opaque component because every host links it. No generalization work is done on it, per the locked decision.
- `tools/setup.py`, `tools/recomp/package.py`, the smoke scripts and the texture-pack tooling remain Populous-specific in M0 and are listed as M2 and M4 debt in the spec amendment.

---

## File Structure

Final layout after Task 2, with the origin of each directory:

```
recomp-kit/
  CMakeLists.txt, CMakePresets.json, Makefile     from SRC, edited
  cmake/                                          from SRC/cmake
  platform/                                       from SRC/src/recomp/platform
  runtime/                                        from SRC/src/recomp/runtime + SRC/tools/recomp/runtime/x86.h
  dx/                                             from SRC/src/recomp/dx
  host/                                           from SRC/src/recomp/host + SRC/src/backends/*/indexed_frame.*
  mods/                                           from SRC/src/recomp/mods (mod foundation)
  mods/native/                                    from SRC/src/recomp/native
  games/populous/game.toml                        new (Task 3)
  games/populous/globals.toml                     from SRC/tools/recomp/symbols/globals.toml
  games/populous/mods/                            from SRC/mods (core, debug, examples, smoke plugins)
  games/populous/assets/terrain/                  from SRC/assets/terrain
  third_party/                                    from SRC/third_party
  tools/                                          from SRC/tools, edited
  tools/game_config.py                            new (Task 3): load a game directory
  tools/gen_game_config.py                        new (Task 3): emit game_config.h and .cmake
  tools/gen_stub_translation.py                   new (Task 7): link-only translation for CI
  tools/check_game_literals.py                    new (Task 4): guard against game literals
  tests/                                          from SRC/tests + new Python tests
  docs/                                           from SRC/docs + this repo's docs/superpowers
  .github/workflows/checks.yml                    from SRC, rewritten in Task 8
```

Responsibilities of the new files:

- `tools/game_config.py`: the one place that knows the schema of `game.toml` and `globals.toml`. Everything else asks it.
- `tools/gen_game_config.py`: pure rendering of a loaded config into C macros and CMake `set()` lines.
- `tools/gen_stub_translation.py`: writes a `table.c` and `funcs.h` that define every symbol the real translation exports, with zero functions.
- `tools/check_game_literals.py`: scans kit code for forbidden tokens on non-comment lines.

---

### Task 1: Import the Populous source tree and establish the baseline

**Files:**
- Create: everything tracked in `SRC` except `translation/` and `.github/workflows/release.yml`
- Modify: `tools/build.py` (remove `--publish-tracked`), `tools/test.py` (portable test list), `src/recomp/host/CMakeLists.txt` (remove `translation_check`), `cmake/Translate.cmake` (remove the tracked fallback), `CHANGELOG.md`
- Delete: `tools/recomp/tests/test_translation.py`, `tools/recomp/tests/test_translate_publish.py`
- Test: `tools/test.py`, `tools/test.py --native`

**Interfaces:**
- Produces: a building, testing copy of the Populous tree inside `KIT`, ignored symlinks `original/` and `analysis/` pointing at the developer's game inputs, and a baseline file `build/baseline/test_translate.txt`.

- [ ] **Step 1: Copy tracked files, excluding the translation and the release workflow**

```bash
cd <workspace>/populous-recomp-checkout
git ls-files -z | grep -zvE '^(translation/|docs/superpowers/|\.github/workflows/release\.yml$)' \
  | rsync -a --files-from=- --from0 ./ <workspace>/recomp-kit/
cd <workspace>/recomp-kit
ls   # expect AGENTS.md CMakeLists.txt cmake docs mods src third_party tools tests ...
test ! -d translation && echo "no translation tracked"
ls docs/superpowers/specs   # only this repo's spec; the checkout's own superpowers docs were excluded
```

- [ ] **Step 2: Link the developer's game inputs (ignored paths)**

```bash
cd <workspace>/recomp-kit
ln -s ../populous-recomp-checkout/original original
ln -s ../populous-recomp/analysis analysis
test -f original/gog/D3DPopTB.exe && test -f analysis/decompiled/D3DPopTB.exe/functions.tsv && echo "inputs linked"
git status --short | grep -E '^\?\? (original|analysis)' && echo "BUG: symlinks not ignored" || echo "symlinks ignored"
```

- [ ] **Step 3: Create the virtual environment**

```bash
cd <workspace>/recomp-kit
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements-dev.txt
.venv/bin/python -c "import capstone, pefile, unicorn, pytest; print('ok')"
```

- [ ] **Step 4: Remove the tracked-translation features**

In `tools/build.py`:
- delete the `publish_tracked` function;
- delete the `--publish-tracked` argument in `parse_args`;
- in `main`, delete the two lines `if args.publish_tracked: args.regenerate = True` and the `if args.publish_tracked: publish_tracked(ROOT)` block.

In `tools/test.py`, remove `"tools/recomp/tests/test_translate_publish.py"` and `"tools/recomp/tests/test_translation.py"` from `PORTABLE_TESTS`.

In `src/recomp/host/CMakeLists.txt`, delete the block:

```cmake
# The tracked translation names the loader's pinned exe and is complete.
add_test(NAME translation_check
  COMMAND ${Python3_EXECUTABLE} -m unittest ${POP_ROOT}/tools/recomp/tests/test_translation.py
  WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(translation_check PROPERTIES LABELS nogame)
```

In `cmake/Translate.cmake`, replace the first block:

```cmake
# The translated game. A developer's fresh regeneration in build/recomp/gen
# wins; otherwise the tracked translation in translation/ (the one supported
# GOG build) is what every host links.
if(EXISTS ${POP_OUT}/gen/table.c)
  set(POP_GEN_DIR ${POP_OUT}/gen)
else()
  set(POP_GEN_DIR ${POP_ROOT}/translation)
endif()
```

with:

```cmake
# The translated game lives only in the developer's build/recomp/gen; the
# kit never tracks generated code (spec section 11).
set(POP_GEN_DIR ${POP_OUT}/gen)
```

Delete the two test files:

```bash
rm tools/recomp/tests/test_translation.py tools/recomp/tests/test_translate_publish.py
```

Also update the `README.md` sentence that starts "The tracked `translation/` directory lets the hosts build" to: "Generated code is never tracked; `tools/build.py --regenerate` produces it from your own game files."

- [ ] **Step 5: Run the portable suites**

Run: `.venv/bin/python tools/test.py`
Expected: pytest reports all passing; no reference to `test_translation` or `test_translate_publish`.

- [ ] **Step 6: Translate and build Populous in the new location**

Run: `.venv/bin/python tools/build.py --regenerate --jobs 8`
Expected: ends with `PopRecomp.app` in `build/`, no "Build failed". This compiles about 1.8 million lines of generated C; allow twenty minutes.

Run: `open build/PopRecomp.app` and confirm the menu appears and a level loads. Quit with Command-Q.

- [ ] **Step 7: Run the native suites and record the baseline**

```bash
.venv/bin/python tools/test.py --native 2>&1 | tail -5
mkdir -p build/baseline
.venv/bin/python tools/recomp/tests/test_translate.py 2>&1 | tail -3 | tee build/baseline/test_translate.txt
cp -R build/recomp/gen build/baseline/gen
```

Expected: ctest reports 100% tests passed; the last lines of `test_translate.py` give a passing count. Keep `build/baseline/` for Tasks 6 and 9.

- [ ] **Step 8: Record the import in the changelog and commit**

Add under `## Unreleased` in `CHANGELOG.md`:

```
- Imported the Populous recompilation from populous-recomp-checkout at
  b450dfa8bae7fe568192ef61028ed82489cba394 as the base of recomp-kit. The
  translation is no longer tracked; regenerate it with tools/build.py --regenerate.
```

```bash
.venv/bin/python tools/format.py
.venv/bin/python tools/check_repo.py
git add -A
git commit -m "Import populous-recomp-checkout b450dfa without the tracked translation"
```

---

### Task 2: Promote the C/C++ tree to the kit layout

**Files:**
- Move: see the mapping in Step 1
- Modify: `CMakeLists.txt`, `cmake/Plugins.cmake`, `cmake/Translate.cmake`, `host/CMakeLists.txt`, `mods/CMakeLists.txt`, `runtime/intrinsics.h`, `dx/tests/host_api_header_test.c`, `mods/tests/api_header_test.c`, `host/present.cpp`, `host/present_pixels.cpp`, `host/indexed_frame.cpp`, `mods/capture_seam.cpp`, `mods/tests/replay_tests_bridge.cpp`, `mods/native/**`, `tools/format.py`, `tools/build.py`, `tools/recomp/build_core.py`, `tools/recomp/translate.py`, `tools/recomp/tests/test_host_boundary.py`, `tools/recomp/tests/test_translate.py`, `README.md`, `CONTRIBUTING.md`, `AGENTS.md`, `docs/*.md`
- Test: `tools/test.py`, `tools/test.py --native`, `tools/build.py`

**Interfaces:**
- Produces: the directory names every later task uses: `runtime/`, `dx/`, `host/`, `platform/`, `mods/`, `games/populous/`.

- [ ] **Step 1: Move directories with git**

```bash
cd <workspace>/recomp-kit
mkdir -p games/populous
git mv mods games/populous/mods
git mv src/recomp/mods mods
git mv src/recomp/native mods/native
git mv src/recomp/runtime runtime
git mv src/recomp/dx dx
git mv src/recomp/host host
git mv src/recomp/platform platform
git mv src/backends/cpu/indexed_frame.cpp host/indexed_frame.cpp
git mv src/backends/indexed_frame.hpp host/indexed_frame.hpp
git mv tools/recomp/runtime/x86.h runtime/x86.h
git mv tools/recomp/symbols/globals.toml games/populous/globals.toml
git mv assets/terrain games/populous/assets/terrain
rmdir src/backends/cpu src/backends src/recomp src tools/recomp/runtime tools/recomp/symbols assets 2>/dev/null; true
git status --short | grep -v '^R' ; echo "(anything above is not a rename)"
```

- [ ] **Step 2: Rewrite CMake paths**

```bash
grep -rl 'src/recomp/' CMakeLists.txt cmake host/CMakeLists.txt runtime/CMakeLists.txt dx/CMakeLists.txt \
  platform/CMakeLists.txt mods/CMakeLists.txt host/gpu/CMakeLists.txt games/populous/mods/CMakeLists.txt \
  | xargs sed -i '' 's|src/recomp/||g'
```

Then edit by hand:

`CMakeLists.txt`, the subdirectory block becomes exactly:

```cmake
add_subdirectory(platform)
add_subdirectory(runtime)
add_subdirectory(dx)
add_subdirectory(mods)
add_subdirectory(games/populous/mods)
add_subdirectory(host)
```

`host/CMakeLists.txt`:
- delete `set(BACKENDS ${POP_ROOT}/src/backends/cpu)`;
- replace `${BACKENDS}/indexed_frame.cpp` with `${HOST}/indexed_frame.cpp`;
- in every `target_include_directories` and in `pop_host`, delete the `${POP_ROOT}/src` entry (run `grep -n 'POP_ROOT}/src\b' host/CMakeLists.txt mods/CMakeLists.txt dx/CMakeLists.txt` and remove each hit).

`mods/CMakeLists.txt`: run `grep -n 'native' mods/CMakeLists.txt` and change every path that now reads `${POP_ROOT}/native/...` to `${POP_ROOT}/mods/native/...`.

`cmake/Translate.cmake`: confirm the include line now reads `${POP_ROOT}/runtime` (the sed did it); it must not mention `tools/recomp/runtime`.

- [ ] **Step 3: Rewrite includes**

```bash
cd <workspace>/recomp-kit
sed -i '' 's|"backends/indexed_frame.hpp"|"indexed_frame.hpp"|' host/present.cpp host/present_pixels.cpp host/indexed_frame.cpp
sed -i '' 's|"src/recomp/dx/host_api.h"|"dx/host_api.h"|' dx/tests/host_api_header_test.c
sed -i '' 's|"src/recomp/mods/pop_mod_api.h"|"mods/pop_mod_api.h"|' mods/tests/api_header_test.c
sed -i '' 's|"tools/recomp/runtime/x86.h"|"x86.h"|' runtime/intrinsics.h
sed -i '' 's|"\.\./native/|"native/|g' mods/capture_seam.cpp
sed -i '' 's|"\.\./\.\./native/|"../native/|g' mods/tests/replay_tests_bridge.cpp
sed -i '' 's|"\.\./mods/pop_mod_api.h"|"../pop_mod_api.h"|' mods/native/replay.h
sed -i '' 's|"\.\./\.\./mods/tests/mods_tests.h"|"../../tests/mods_tests.h"|; s|"\.\./\.\./mods/mods_internal.h"|"../../mods_internal.h"|' mods/native/tests/*.cpp
# native/ moved one level deeper: its includes of sibling components gain one "../"
sed -i '' 's|"\.\./\(runtime\|dx\|platform\|host\)/|"../../\1/|g' mods/native/*.cpp mods/native/*.h
sed -i '' 's|"\.\./\.\./\(runtime\|dx\|platform\|host\)/|"../../../\1/|g' mods/native/tests/*.cpp
grep -rn '#include "' mods/native | grep -vE '"(\.\./)*(runtime|dx|platform|host|tests|native|pop_mod_api|mods_internal|page_track|replay|shim_capture)' ; echo "(review any line above)"
```

- [ ] **Step 4: Rewrite Python and documentation paths**

```bash
cd <workspace>/recomp-kit
sed -i '' 's|"src", "mods", "tests", "tools/recomp/runtime"|"runtime", "dx", "host", "platform", "mods", "games", "tests"|' tools/format.py
sed -i '' 's|ROOT / "src" / "recomp" / "host"|ROOT / "host"|' tools/recomp/tests/test_host_boundary.py
sed -i '' 's|root / "tools/recomp/runtime/x86.h"|root / "runtime/x86.h"|; s|assets/terrain/materials-v1.png|games/populous/assets/terrain/materials-v1.png|' tools/build.py
sed -i '' 's|"src/recomp/runtime"|"runtime"|; s|"tools/recomp/runtime/x86.h"|"runtime/x86.h"|' tools/recomp/tests/test_translate.py
sed -i '' 's|ROOT / "src/recomp/mods"|ROOT / "mods"|; s|ROOT / "mods/core"|ROOT / "games/populous/mods/core"|' tools/recomp/build_core.py
sed -i '' 's|tools/recomp/symbols/globals.toml|games/populous/globals.toml|' tools/recomp/translate.py
grep -rl 'src/recomp/\|tools/recomp/runtime\|tools/recomp/symbols' tools tests README.md CONTRIBUTING.md AGENTS.md docs/*.md \
  | xargs sed -i '' 's|src/recomp/native|mods/native|g; s|src/recomp/||g; s|tools/recomp/runtime/x86.h|runtime/x86.h|g; s|tools/recomp/symbols/globals.toml|games/populous/globals.toml|g'
```

- [ ] **Step 5: Verify no stale path remains**

Run:

```bash
grep -rn 'src/recomp\|src/backends\|tools/recomp/runtime\|tools/recomp/symbols\|assets/terrain' \
  --exclude-dir=.git --exclude-dir=build --exclude-dir=.venv --exclude-dir=superpowers --exclude=CHANGELOG.md . \
  | grep -v 'games/populous/assets/terrain'
```

Expected: no output. Fix any hit by hand.

- [ ] **Step 6: Build, test, format**

Run: `.venv/bin/python tools/build.py --jobs 8`
Expected: configure succeeds (it reuses `build/recomp/gen` from Task 1), `build/PopRecomp.app` relinks.

Run: `.venv/bin/python tools/test.py --native`
Expected: 100% tests passed, including `host_boundary_check`, `dx_tests`, `runtime_tests`, `mods_tests` if built.

Run: `.venv/bin/python tools/test.py` and `.venv/bin/python tools/format.py` and `.venv/bin/python tools/check_repo.py`
Expected: all clean. If `format.py` reports differences, run it with `--write` and re-run.

Run: `open build/PopRecomp.app`, load a level, quit.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "Promote the C/C++ tree to the kit layout

runtime/, dx/, host/, platform/, mods/ (with native/ inside) at the top
level; Populous plugins, curated globals and artwork under games/populous/."
```

---

### Task 3: Game config directory, generator, and CMake wiring

**Files:**
- Create: `games/populous/game.toml`, `tools/game_config.py`, `tools/gen_game_config.py`, `tests/test_game_config.py`
- Modify: `CMakeLists.txt`, `cmake/MacBundle.cmake`, `host/CMakeLists.txt`, `runtime/x86.h`, `tools/build.py`, `tools/test.py`, `tests/test_build_py.py`, `tools/requirements.txt`
- Test: `tests/test_game_config.py`, `tests/test_build_py.py`, `tools/test.py --native`

**Interfaces:**
- Produces: `game_config.load(game_dir) -> dict` with keys `game`, `translate`, `hooks`, `globals`, `dir`, `source`; macros `RECOMP_GAME_ID`, `RECOMP_GAME_NAME`, `RECOMP_APP_NAME`, `RECOMP_BUNDLE_ID`, `RECOMP_EXECUTABLE`, `RECOMP_EXE_SHA256`, `RECOMP_GUEST_ROOT`, `RECOMP_DEVELOPER_EXE` (C strings), `RECOMP_IMAGE_BASE`, `RECOMP_ENTRY_POINT` (`0x...u`), `RECOMP_HOOK_<NAME>` (scalar `0x...u`, or a brace list plus `RECOMP_HOOK_<NAME>_COUNT`), `RECOMP_GLOBAL_<NAME>_ADDR` and optional `_SIZE`, `_STRIDE`, `_COUNT`; CMake variables `RECOMP_GAME_ID`, `RECOMP_GAME_NAME`, `RECOMP_APP_NAME`, `RECOMP_BUNDLE_ID`, `RECOMP_EXECUTABLE`, `RECOMP_IMAGE_BASE`; CMake cache variable `RECOMP_GAME` (default `populous`); CMake target `recomp_app` (was `PopRecomp`); `tools/build.py --game <id>`.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_game_config.py`:

```python
"""tools/game_config.py loads a game directory; tools/gen_game_config.py renders it."""

import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load_module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


game_config = load_module("game_config")
gen_game_config = load_module("gen_game_config")


class LoadTests(unittest.TestCase):
    def test_populous_loads_with_every_required_key(self):
        cfg = game_config.load(ROOT / "games/populous")
        self.assertEqual(cfg["game"]["id"], "populous")
        self.assertEqual(cfg["game"]["executable"], "D3DPopTB.exe")
        self.assertEqual(cfg["game"]["entry_point"], 0x0055D6C0)
        self.assertEqual(cfg["game"]["image_base"], 0x00400000)
        self.assertIn("simulation_turn", cfg["globals"])
        self.assertEqual(cfg["globals"]["simulation_turn"]["addr"], 0x0089D188)

    def test_missing_key_is_an_error_naming_the_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp)
            (game / "game.toml").write_text('[game]\nid = "x"\n')
            (game / "globals.toml").write_text("")
            with self.assertRaises(ValueError) as caught:
                game_config.load(game)
            self.assertIn("game.toml", str(caught.exception))
            self.assertIn("sha256", str(caught.exception))


class RenderTests(unittest.TestCase):
    def setUp(self):
        self.cfg = game_config.load(ROOT / "games/populous")
        self.header = gen_game_config.render_header(self.cfg)
        self.cmake = gen_game_config.render_cmake(self.cfg)

    def test_identity_macros(self):
        self.assertIn('#define RECOMP_GAME_ID "populous"', self.header)
        self.assertIn('#define RECOMP_APP_NAME "PopRecomp"', self.header)
        self.assertIn('#define RECOMP_EXECUTABLE "D3DPopTB.exe"', self.header)
        self.assertIn('#define RECOMP_EXE_SHA256 "815ba8a550f571c38b602cf3386f65aab942667a4a2d9c7096b3660deac2eacd"',
                      self.header)
        self.assertIn('#define RECOMP_GUEST_ROOT "C:\\\\Populous"', self.header)
        self.assertIn("#define RECOMP_ENTRY_POINT 0x0055d6c0u", self.header)
        self.assertIn("#define RECOMP_IMAGE_BASE 0x00400000u", self.header)

    def test_globals_macros(self):
        self.assertIn("#define RECOMP_GLOBAL_SIMULATION_TURN_ADDR 0x0089d188u", self.header)
        self.assertIn("#define RECOMP_GLOBAL_ENTITY_BASE_STRIDE 179u", self.header)
        self.assertIn("#define RECOMP_GLOBAL_ENTITY_BASE_COUNT 2000u", self.header)

    def test_hook_lists_render_as_brace_lists_with_a_count(self):
        cfg = dict(self.cfg, hooks={"pair": [0x10, 0x20], "one": 0x30})
        header = gen_game_config.render_header(cfg)
        self.assertIn("#define RECOMP_HOOK_PAIR_COUNT 2", header)
        self.assertIn("#define RECOMP_HOOK_PAIR {0x00000010u, 0x00000020u}", header)
        self.assertIn("#define RECOMP_HOOK_ONE 0x00000030u", header)

    def test_cmake_fragment(self):
        self.assertIn('set(RECOMP_APP_NAME "PopRecomp")', self.cmake)
        self.assertIn('set(RECOMP_GAME_NAME "Populous: The Beginning")', self.cmake)
        self.assertIn("set(RECOMP_IMAGE_BASE 0x00400000u)", self.cmake)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `.venv/bin/python -m pytest -q tests/test_game_config.py`
Expected: FAIL at import, "No such file" for `tools/game_config.py`.

- [ ] **Step 3: Add the TOML dependency**

Append to `tools/requirements.txt`:

```
tomli==2.2.1
```

Run: `.venv/bin/python -m pip install -r requirements-dev.txt`

- [ ] **Step 4: Write `games/populous/game.toml`**

```toml
# games/populous/game.toml - everything the kit needs to know about this game
# that is not derivable from the binary. Addresses are guest addresses in the
# pinned executable. Hooks are added in later tasks as literals leave the code.

[game]
id = "populous"
name = "Populous: The Beginning"
app_name = "PopRecomp"
bundle_id = "dev.recompkit.populous"
executable = "D3DPopTB.exe"
sha256 = "815ba8a550f571c38b602cf3386f65aab942667a4a2d9c7096b3660deac2eacd"
image_base = 0x00400000
entry_point = 0x0055d6c0
guest_root = 'C:\Populous'
# Where a developer checkout finds the game, relative to the repository root.
developer_exe = "original/gog/D3DPopTB.exe"

[translate]
# Ghidra listing directory holding functions.tsv and functions/*.asm,
# relative to the repository root.
listings = "analysis/decompiled/D3DPopTB.exe"
# Curated symbols, relative to this directory.
globals = "globals.toml"
# The guest global whose reads select a visual phase (sprite_animation_counter).
animation_counter = 0x00897981
# Reads of animation_counter that only pick a visual phase or blink. Do NOT
# include interpolation, simulation stamps, FPS measurement or input timing.
volatile_reads = [
  0x468f27, 0x4690ad, 0x46922b, 0x469327,  # spell/particle textures
  0x4758b0, 0x4758c4, 0x4758d4,            # selection pulse
  0x525c05, 0x525c15,                      # rectangle pulse
  0x49d2eb, 0x49d386, 0x49d890, 0x4a2581,  # UI blinking
  0x47a76d, 0x47a8f5,                      # palette animation
  0x50f45c,                                # effect sprite phase
]

[hooks]
```

- [ ] **Step 5: Write `tools/game_config.py`**

```python
"""Load a game directory: games/<id>/game.toml plus its curated globals file.

This module is the only place that knows the schema. Python 3.9 has no
tomllib, so the pinned tomli is the fallback."""

from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    import tomli as tomllib

REQUIRED_GAME_KEYS = ("id", "name", "app_name", "bundle_id", "executable", "sha256",
                      "image_base", "entry_point", "guest_root", "developer_exe")


def load(game_dir):
    """Return the parsed config with `globals` merged in and `dir`/`source` recorded."""
    game_dir = Path(game_dir)
    source = game_dir / "game.toml"
    with source.open("rb") as fh:
        cfg = tomllib.load(fh)
    game = cfg.get("game", {})
    missing = [key for key in REQUIRED_GAME_KEYS if key not in game]
    if missing:
        raise ValueError("%s: missing [game] keys: %s" % (source, ", ".join(missing)))
    translate = cfg.setdefault("translate", {})
    cfg.setdefault("hooks", {})
    globals_path = game_dir / translate.get("globals", "globals.toml")
    with globals_path.open("rb") as fh:
        cfg["globals"] = tomllib.load(fh).get("globals", {})
    cfg["dir"] = game_dir
    cfg["source"] = str(source)
    return cfg
```

- [ ] **Step 6: Write `tools/gen_game_config.py`**

```python
#!/usr/bin/env python3
"""Render games/<id>/game.toml into game_config.h and game_config.cmake.

    tools/gen_game_config.py --game-dir games/populous --header OUT.h --cmake OUT.cmake

The header is what runtime/, dx/ and host/ include for every game-specific
value; the cmake fragment names the app and the image base for the build."""

import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import game_config  # noqa: E402

STRINGS = (("id", "RECOMP_GAME_ID"), ("name", "RECOMP_GAME_NAME"), ("app_name", "RECOMP_APP_NAME"),
           ("bundle_id", "RECOMP_BUNDLE_ID"), ("executable", "RECOMP_EXECUTABLE"),
           ("sha256", "RECOMP_EXE_SHA256"), ("guest_root", "RECOMP_GUEST_ROOT"),
           ("developer_exe", "RECOMP_DEVELOPER_EXE"))
ADDRESSES = (("image_base", "RECOMP_IMAGE_BASE"), ("entry_point", "RECOMP_ENTRY_POINT"))


def c_string(value):
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def c_hex(value):
    return "0x%08xu" % value


def render_header(cfg):
    game = cfg["game"]
    lines = ["/* generated by tools/gen_game_config.py from %s -- do not edit */" % cfg["source"],
             "#pragma once"]
    for key, macro in STRINGS:
        lines.append("#define %s %s" % (macro, c_string(game[key])))
    for key, macro in ADDRESSES:
        lines.append("#define %s %s" % (macro, c_hex(game[key])))
    for key, value in sorted(cfg.get("hooks", {}).items()):
        macro = "RECOMP_HOOK_" + key.upper()
        if isinstance(value, list):
            lines.append("#define %s_COUNT %d" % (macro, len(value)))
            lines.append("#define %s {%s}" % (macro, ", ".join(c_hex(v) for v in value)))
        else:
            lines.append("#define %s %s" % (macro, c_hex(value)))
    for key, entry in sorted(cfg.get("globals", {}).items()):
        macro = "RECOMP_GLOBAL_" + key.upper()
        lines.append("#define %s_ADDR %s" % (macro, c_hex(entry["addr"])))
        for field in ("size", "stride", "count"):
            if field in entry:
                lines.append("#define %s_%s %du" % (macro, field.upper(), entry[field]))
    return "\n".join(lines) + "\n"


def render_cmake(cfg):
    game = cfg["game"]
    lines = ["# generated by tools/gen_game_config.py from %s -- do not edit" % cfg["source"]]
    for key, macro in STRINGS[:5]:
        lines.append('set(%s "%s")' % (macro, game[key]))
    lines.append("set(RECOMP_IMAGE_BASE %s)" % c_hex(game["image_base"]))
    return "\n".join(lines) + "\n"


def write_if_changed(path, text):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.is_file() or path.read_text() != text:
        path.write_text(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--cmake", type=Path, required=True)
    args = parser.parse_args()
    cfg = game_config.load(args.game_dir)
    write_if_changed(args.header, render_header(cfg))
    write_if_changed(args.cmake, render_cmake(cfg))


if __name__ == "__main__":
    main()
```

- [ ] **Step 7: Run the tests to see them pass**

Run: `.venv/bin/python -m pytest -q tests/test_game_config.py`
Expected: 6 passed.

- [ ] **Step 8: Wire the generator into CMake**

In `CMakeLists.txt`, move the `find_package(Python3 ...)` block above the `configure_file(... version.h ...)` line, then replace the two lines

```cmake
set(POP_RECOMP_APP_NAME PopRecomp CACHE STRING "macOS bundle name")
```

and

```cmake
if(NOT POP_RECOMP_APP_NAME MATCHES "^[A-Za-z0-9_-]+$")
  message(FATAL_ERROR "Invalid POP_RECOMP_APP_NAME: ${POP_RECOMP_APP_NAME}")
endif()
```

with this block placed right after the Python3 lookup:

```cmake
# The game this build is for. games/<RECOMP_GAME>/game.toml is rendered into
# generated/game_config.h (C macros) and generated/game_config.cmake (build
# names) at configure time; editing the toml reconfigures.
set(RECOMP_GAME "populous" CACHE STRING "Directory under games/ whose game.toml configures this build")
set(RECOMP_GAME_DIR ${POP_ROOT}/games/${RECOMP_GAME})
if(NOT EXISTS ${RECOMP_GAME_DIR}/game.toml)
  message(FATAL_ERROR "No game config at ${RECOMP_GAME_DIR}/game.toml")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${RECOMP_GAME_DIR}/game.toml ${RECOMP_GAME_DIR}/globals.toml
  ${POP_ROOT}/tools/gen_game_config.py ${POP_ROOT}/tools/game_config.py)
execute_process(
  COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/tools/gen_game_config.py
          --game-dir ${RECOMP_GAME_DIR}
          --header ${CMAKE_BINARY_DIR}/generated/game_config.h
          --cmake ${CMAKE_BINARY_DIR}/generated/game_config.cmake
  RESULT_VARIABLE RECOMP_GAME_CONFIG_RESULT)
if(NOT RECOMP_GAME_CONFIG_RESULT EQUAL 0)
  message(FATAL_ERROR "tools/gen_game_config.py failed for ${RECOMP_GAME_DIR}")
endif()
include(${CMAKE_BINARY_DIR}/generated/game_config.cmake)
if(NOT RECOMP_APP_NAME MATCHES "^[A-Za-z0-9_-]+$")
  message(FATAL_ERROR "Invalid app_name in ${RECOMP_GAME_DIR}/game.toml: ${RECOMP_APP_NAME}")
endif()
include_directories(${CMAKE_BINARY_DIR}/generated)
add_compile_definitions(GUEST_IMAGE_BASE=${RECOMP_IMAGE_BASE})
```

In `cmake/MacBundle.cmake`, replace every `POP_RECOMP_APP_NAME` with `RECOMP_APP_NAME`.

In `host/CMakeLists.txt`, rename the host target: `pop_host(PopRecomp GEN UI_LAYER OPT 2` becomes `pop_host(recomp_app GEN UI_LAYER OPT 2`, the following `pop_link_sdl(PopRecomp)` becomes `pop_link_sdl(recomp_app)`, and the `if(APPLE) pop_mac_bundle(PopRecomp) endif()` becomes:

```cmake
  set_target_properties(recomp_app PROPERTIES OUTPUT_NAME ${RECOMP_APP_NAME})
  if(APPLE)
    pop_mac_bundle(recomp_app)
  endif()
```

Run `grep -rn PopRecomp cmake host/CMakeLists.txt CMakeLists.txt` and change any remaining target reference to `recomp_app`; leave string literals in C++ alone (Task 4 handles them).

In `runtime/x86.h`, change line 45 from

```c
#define GUEST_IMAGE_BASE 0x00400000u
```

to

```c
#ifndef GUEST_IMAGE_BASE
#define GUEST_IMAGE_BASE 0x00400000u /* the build passes the game's base; this default serves the harness */
#endif
```

- [ ] **Step 9: Teach `tools/build.py` and `tools/test.py` about `--game`**

In `tools/build.py`:
- after `sys.path.insert(0, str(ROOT / "tools/recomp"))` add `sys.path.insert(0, str(ROOT / "tools"))` and `import game_config`;
- change `"app": ["PopRecomp"]` to `"app": ["recomp_app"]`;
- change `archive_path(root=ROOT, system=None)` to `archive_path(root=ROOT, system=None, game="populous")` and inside it derive the app name with `name = game_config.load(Path(root) / "games" / game)["game"]["app_name"]`, replacing the literal `PopRecomp` in the paths it returns;
- in `configure(preset, extra=())`, keep the signature; in `main`, call `configure(preset, ["-DRECOMP_GAME=" + args.game])` and make `configure` append `list(extra)` to the command;
- in `parse_args`, add `parser.add_argument("--game", default="populous", help="Directory under games/")` and validate `(ROOT / "games" / args.game / "game.toml").is_file()` with `parser.error` otherwise;
- in `main`, replace the literal `original/gog/D3DPopTB.exe` check with `cfg = game_config.load(ROOT / "games" / args.game)` and `(ROOT / cfg["game"]["developer_exe"]).is_file()`, and the literal `analysis/decompiled/D3DPopTB.exe/functions.tsv` with `(ROOT / cfg["translate"]["listings"] / "functions.tsv")`;
- in `texture_pack`, wrap the body in `if artwork.is_file():` so a game without artwork skips it, using `artwork = ROOT / "games" / game / "assets/terrain/materials-v1.png"` (pass `args.game` in).

In `tools/test.py`:
- add `parser.add_argument("--game", default="populous")`;
- replace the literal `original/gog/D3DPopTB.exe` check with the same `game_config` lookup (import it the way `build.py` does);
- pass `game=args.game` to `build_py.archive_path()`.

Add to `tests/test_build_py.py`:

```python
    def test_game_defaults_to_populous_and_is_validated(self):
        args, _ = build_py.parse_args([], system="Darwin")
        self.assertEqual(args.game, "populous")
        with self.assertRaises(SystemExit):
            build_py.parse_args(["--game", "no-such-game"], system="Darwin")

    def test_archive_path_uses_the_configured_app_name(self):
        self.assertTrue(str(build_py.archive_path(system="Darwin", game="populous")).endswith("PopRecomp.app"))
```

- [ ] **Step 10: Run tests, rebuild, commit**

Run: `.venv/bin/python tools/test.py`
Expected: all portable suites pass, including the two new `test_build_py` cases.

Run: `.venv/bin/python tools/build.py --jobs 8`
Expected: configure prints nothing about a missing game config; `build/PopRecomp.app` builds; `build/cmake/macos/generated/game_config.h` exists.

Run: `.venv/bin/python tools/test.py --native`
Expected: 100% tests passed.

```bash
.venv/bin/python tools/format.py --write
git add -A
git commit -m "Add games/populous/game.toml and generate game_config.h at configure time"
```

---

### Task 4: Replace identity literals with generated macros

**Files:**
- Create: `tools/check_game_literals.py`, `tests/test_game_literals.py`, `host/Info.plist.in`
- Modify: `runtime/loader.h`, `runtime/loader.cpp`, `runtime/kernel32.cpp`, `runtime/win32.h` (only if it holds code), `runtime/layout.cpp`, `host/game_path.cpp`, `host/sdl/main.cpp`, `host/gpu/vulkan/vulkan_device.cpp`, `dx/ddraw.cpp`, `cmake/MacBundle.cmake`, `tools/recomp/finish_bundle.py`, `tools/test.py`
- Delete: `host/Info.plist`
- Test: `tests/test_game_literals.py`, `tools/test.py --native`

**Interfaces:**
- Consumes: the `RECOMP_*` macros from Task 3.
- Produces: `tools/check_game_literals.py` with `main() -> int` (0 clean, 1 with findings printed as `path:line: token`).

- [ ] **Step 1: Write the guard and its test, and watch it fail**

Create `tools/check_game_literals.py`:

```python
#!/usr/bin/env python3
"""Fail when a Populous literal appears in kit code on a non-comment line.

The kit's runtime, shims, hosts and platform layer must not know which game
they are building; games/<id>/game.toml does. Tests are exempt, because the
game-backed suites assert Populous behaviour on purpose."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
DIRECTORIES = ("runtime", "dx", "host", "platform")
SUFFIXES = {".c", ".cpp", ".h", ".hpp", ".mm", ".in"}
TOKENS = ("D3DPopTB", "C:\\\\Populous", "PopRecomp", '"Populous', "original/gog")
# The Populous parity fixture pins guest frames and addresses on purpose. It
# moves under games/populous/ in M2; until then it is the one exemption.
EXEMPT = {"runtime/fixture.cpp", "host/fixture_view.h"}


def code_lines(text):
    """Yield (line number, code) with block and line comments removed."""
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    for number, line in enumerate(text.splitlines(), 1):
        yield number, line.split("//", 1)[0]


def findings():
    for directory in DIRECTORIES:
        for path in sorted((ROOT / directory).rglob("*")):
            relative = path.relative_to(ROOT)
            if path.suffix not in SUFFIXES or "tests" in relative.parts or str(relative) in EXEMPT:
                continue
            for number, code in code_lines(path.read_text(errors="replace")):
                for token in TOKENS:
                    if token in code:
                        yield "%s:%d: %s" % (path.relative_to(ROOT), number, token)


def main():
    found = list(findings())
    for line in found:
        print(line)
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
```

Create `tests/test_game_literals.py`:

```python
"""No game-specific literal in kit code; see tools/check_game_literals.py."""

import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("check_game_literals",
                                              Path(__file__).resolve().parents[1] / "tools/check_game_literals.py")
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)


class GameLiteralTests(unittest.TestCase):
    def test_kit_code_has_no_game_literals(self):
        self.assertEqual(list(check.findings()), [])

    def test_comments_are_ignored(self):
        lines = dict(check.code_lines("int a; // D3DPopTB\n/* PopRecomp\n */ int b;\n"))
        self.assertNotIn("D3DPopTB", lines[1])
        self.assertNotIn("PopRecomp", lines[2])
        self.assertIn("int b;", lines[3])
```

Add `"tests/test_game_literals.py"` and `"tests/test_game_config.py"` to `PORTABLE_TESTS` in `tools/test.py`.

Run: `.venv/bin/python -m pytest -q tests/test_game_literals.py`
Expected: `test_kit_code_has_no_game_literals` FAILS with a list that includes `runtime/loader.cpp`, `runtime/kernel32.cpp`, `host/sdl/main.cpp`, `host/game_path.cpp`, `runtime/layout.cpp`, `host/gpu/vulkan/vulkan_device.cpp`, `dx/ddraw.cpp`, `host/Info.plist.in` is not yet present. `test_comments_are_ignored` passes.

- [ ] **Step 2: Loader identity**

`runtime/loader.h`: add `#include "game_config.h"` after the existing includes; change

```cpp
static const uint32_t LOADER_EXPECTED_ENTRY = 0x0055d6c0u;
```

to

```cpp
static const uint32_t LOADER_EXPECTED_ENTRY = RECOMP_ENTRY_POINT;
```

and change the comment on `LOADER_DEFAULT_EXE` to `// RECOMP_DEVELOPER_EXE from game.toml`.

`runtime/loader.cpp`:

```cpp
const char *const LOADER_DEFAULT_EXE = RECOMP_DEVELOPER_EXE;
...
const char *const LOADER_EXPECTED_SHA256 = RECOMP_EXE_SHA256;
```

- [ ] **Step 3: Guest filesystem identity in kernel32**

In `runtime/kernel32.cpp` add `#include "game_config.h"` and apply, line by line (numbers as of the import):

- 60: `std::string g_cur_dir = RECOMP_GUEST_ROOT;`
- 370: `std::string out = RECOMP_GUEST_ROOT;`
- 406: `g_cur_dir = RECOMP_GUEST_ROOT;`
- 420: `modules()[lower(std::string(RECOMP_EXECUTABLE))] = IMAGE_BASE;`
- 1068: `std::string path = RECOMP_GUEST_ROOT "\\" RECOMP_EXECUTABLE;`
- 1072: `path = RECOMP_GUEST_ROOT "\\" + kv.first;`
- 1219: `g_cmdline_addr = guest_strdup(RECOMP_GUEST_ROOT "\\" RECOMP_EXECUTABLE);`
- 1225: `static const char *const k_environment[] = {"PATH=" RECOMP_GUEST_ROOT, "windir=C:\\WINDOWS", nullptr};`

Then find the path normalization that drops the leading root directory name (`win32.h` line 36 describes it; run `grep -n -i '"populous"' runtime/kernel32.cpp runtime/win32.h runtime/misc.cpp`). Replace each literal with the last component of the guest root, provided once in `runtime/win32.h`:

```cpp
// The guest root's own directory name ("Populous" for C:\Populous), so
// C:\<root>\data\x and \data\x resolve to the same host file.
static inline const char *win32_guest_root_name() {
    const char *root = RECOMP_GUEST_ROOT;
    const char *slash = strrchr(root, '\\');
    return slash ? slash + 1 : root;
}
```

(add `#include <string.h>` and `#include "game_config.h"` to `win32.h` if not present). Where the code compared a lower-cased component against `"populous"`, compare against `lower(win32_guest_root_name())` using the file's existing `lower` helper.

- [ ] **Step 4: Host identity**

- `runtime/layout.cpp` line 65: `os_user_data_dir(RECOMP_APP_NAME, buf, sizeof buf)`; add `#include "game_config.h"`.
- `host/game_path.cpp`: `std::string candidate = l.checkout_root + "/" + RECOMP_DEVELOPER_EXE;`; add the include.
- `host/gpu/vulkan/vulkan_device.cpp` line 67: `app.pApplicationName = RECOMP_APP_NAME;`; add the include.
- `dx/ddraw.cpp` line 4065: `gm_put_str(out + DDDEVID_OFF_szDescription, RECOMP_APP_NAME " recompilation device", 512);`; add the include.
- `host/sdl/main.cpp`: add the include, then

```bash
sed -i '' 's|"PopRecomp: |RECOMP_APP_NAME ": |g; s|"PopRecomp %s (%s)\\n"|RECOMP_APP_NAME " %s (%s)\\n"|' host/sdl/main.cpp
```

and by hand:
  - the window title `"Populous: The Beginning"` in `SDL_CreateWindow(...)` becomes `RECOMP_GAME_NAME`;
  - the message-box title `"Populous: The Beginning"` (near line 1004) becomes `RECOMP_GAME_NAME`;
  - the dialog filter `{"Populous executable (D3DPopTB.exe)", "exe"}` becomes `{RECOMP_GAME_NAME " executable (" RECOMP_EXECUTABLE ")", "exe"}`;
  - `"... Pass --exe <path to D3DPopTB.exe> "` becomes `"... Pass --exe <path to " RECOMP_EXECUTABLE "> "`;
  - `"This is not the supported GOG build of D3DPopTB.exe.\n\nExpected "` becomes `"This is not the supported build of " RECOMP_EXECUTABLE ".\n\nExpected "`;
  - `"usage: PopRecomp [--exe <D3DPopTB.exe>] [--version] [--probe-layout]\n"` becomes `"usage: " RECOMP_APP_NAME " [--exe <" RECOMP_EXECUTABLE ">] [--version] [--probe-layout]\n"`.

- [ ] **Step 5: Bundle identity from the config**

```bash
git mv host/Info.plist host/Info.plist.in
sed -i '' 's|PopRecomp|@RECOMP_APP_NAME@|g; s|Populous: The Beginning|@RECOMP_GAME_NAME@|g' host/Info.plist.in
grep -n -A1 CFBundleIdentifier host/Info.plist.in
```

Set the `<string>` after `CFBundleIdentifier` to `@RECOMP_BUNDLE_ID@`.

In `cmake/MacBundle.cmake`, before `set_target_properties`, add:

```cmake
  configure_file(${POP_ROOT}/host/Info.plist.in ${CMAKE_BINARY_DIR}/generated/Info.plist @ONLY)
```

and change `MACOSX_BUNDLE_INFO_PLIST ${POP_ROOT}/host/Info.plist` to `MACOSX_BUNDLE_INFO_PLIST ${CMAKE_BINARY_DIR}/generated/Info.plist`.

In `tools/recomp/finish_bundle.py`, reduce `rename_identity` to the version update only:

```python
def rename_identity(plist_path, name, version):
    """Stamp the version; the bundle's identity comes from generated/Info.plist."""
    data = plistlib.loads(plist_path.read_bytes())
    if version:
        data.update(CFBundleShortVersionString=version.lstrip("v"), CFBundleVersion=version.lstrip("v"))
    plist_path.write_bytes(plistlib.dumps(data))
```

- [ ] **Step 6: Run the guard, build, test**

Run: `.venv/bin/python -m pytest -q tests/test_game_literals.py`
Expected: 2 passed. If a finding remains, it is a line this task missed; fix it the same way.

Run: `.venv/bin/python tools/build.py --jobs 8 && .venv/bin/python tools/test.py --native && .venv/bin/python tools/test.py`
Expected: all pass. `open build/PopRecomp.app`: the window title still reads "Populous: The Beginning" and a level loads. Run `defaults read "$PWD/build/PopRecomp.app/Contents/Info.plist" CFBundleIdentifier` and expect `dev.recompkit.populous`.

- [ ] **Step 7: Commit**

```bash
.venv/bin/python tools/format.py --write
git add -A
git commit -m "Take game identity from game_config.h and guard against game literals"
```

---

### Task 5: Move hook addresses into the game config

**Files:**
- Modify: `games/populous/game.toml`, `runtime/kernel32.cpp`, `dx/ddraw.cpp`, `host/input_gate.cpp`, `host/script.cpp`, `host/smoke_main.cpp`, `tools/check_game_literals.py`, `tests/test_game_config.py`
- Test: `tests/test_game_config.py`, `tools/test.py --native`, `tools/test.py --gameplay`

**Interfaces:**
- Consumes: `RECOMP_HOOK_*` and `RECOMP_GLOBAL_*` macros.
- Produces: `[hooks]` keys `frame_clock_begin`, `frame_clock_wait`, `frame_clock_wait_clamp`, `frame_clock_clamp_deadline`, `frame_clock_wait_deadline`, `cursor_surface_ptrs`, `mouse_vtable`, `mouse_device_ptr`, `mouse_device_right`, `camera`.

- [ ] **Step 1: Extend the config test and watch it fail**

Add to `RenderTests` in `tests/test_game_config.py`:

```python
    def test_populous_hooks_render(self):
        self.assertIn("#define RECOMP_HOOK_FRAME_CLOCK_BEGIN 0x004a45a3u", self.header)
        self.assertIn("#define RECOMP_HOOK_CURSOR_SURFACE_PTRS_COUNT 2", self.header)
        self.assertIn("#define RECOMP_HOOK_CURSOR_SURFACE_PTRS {0x005d5718u, 0x005d571cu}", self.header)
        self.assertIn("#define RECOMP_HOOK_MOUSE_VTABLE 0x00591b6cu", self.header)
        self.assertIn("#define RECOMP_HOOK_CAMERA 0x0074a350u", self.header)
```

Run: `.venv/bin/python -m pytest -q tests/test_game_config.py -k hooks`
Expected: FAIL, the macros are absent.

- [ ] **Step 2: Fill `[hooks]` in `games/populous/game.toml`**

```toml
[hooks]
# runtime/kernel32.cpp native_frame_clock: the two draw-loop waits in the
# pinned executable, identified by the return address of their GetTickCount.
frame_clock_begin = 0x004a45a3          # before the 1000 / DrawFrameRateLimit read
frame_clock_wait = 0x004a47c1           # the wait that only retires the deadline
frame_clock_wait_clamp = 0x004a47a4     # the wait whose successor clamps the rate into EDI
frame_clock_clamp_deadline = 0x0098e7cc # deadline value returned after the clamping wait
frame_clock_wait_deadline = 0x0098e7e0  # deadline value returned after the plain wait
# dx/ddraw.cpp: globals holding the two 32x32 mouse-pointer surface interfaces
cursor_surface_ptrs = [0x005d5718, 0x005d571c]
# host/input_gate.cpp: the game's mouse device object and its vtable
mouse_vtable = 0x00591b6c
mouse_device_ptr = 0x00d0595c
mouse_device_right = 0x00d0599c
# host/smoke_main.cpp: the camera record the smoke script inspects
camera = 0x0074a350
```

Run: `.venv/bin/python -m pytest -q tests/test_game_config.py`
Expected: all pass.

- [ ] **Step 3: Use the macros**

`runtime/kernel32.cpp`, in `native_frame_clock`:

```cpp
    if (caller != RECOMP_HOOK_FRAME_CLOCK_BEGIN && caller != RECOMP_HOOK_FRAME_CLOCK_WAIT &&
        caller != RECOMP_HOOK_FRAME_CLOCK_WAIT_CLAMP)
        return 0;
    ...
    if (caller == RECOMP_HOOK_FRAME_CLOCK_BEGIN) {
    ...
    } else if (active && (caller == RECOMP_HOOK_FRAME_CLOCK_WAIT || caller == RECOMP_HOOK_FRAME_CLOCK_WAIT_CLAMP)) {
    ...
        if (caller == RECOMP_HOOK_FRAME_CLOCK_WAIT_CLAMP) {
            ...
            c->r[R_EDI] = uint32_t(active);
            return RECOMP_HOOK_FRAME_CLOCK_CLAMP_DEADLINE;
        }
        return RECOMP_HOOK_FRAME_CLOCK_WAIT_DEADLINE;
```

`dx/ddraw.cpp` line 1064:

```cpp
const uint32_t kCursorSurfacePtr[RECOMP_HOOK_CURSOR_SURFACE_PTRS_COUNT] = RECOMP_HOOK_CURSOR_SURFACE_PTRS;
```

and check with `grep -n kCursorSurfacePtr dx/ddraw.cpp` that every use iterates with `RECOMP_HOOK_CURSOR_SURFACE_PTRS_COUNT` or `sizeof` rather than a literal 2.

`host/input_gate.cpp` (add `#include "game_config.h"`):

```cpp
    if (p.vtable != RECOMP_HOOK_MOUSE_VTABLE)
...
    if (g_mem && gm_valid(RECOMP_HOOK_MOUSE_DEVICE_PTR, 0x48) &&
        rd32(RECOMP_HOOK_MOUSE_DEVICE_PTR) == RECOMP_HOOK_MOUSE_VTABLE) {
        int right = int32_t(rd32(RECOMP_HOOK_MOUSE_DEVICE_RIGHT));
```

`host/script.cpp` line 13 (add the include):

```cpp
    const uint32_t value = guest_u32(is_turn ? RECOMP_GLOBAL_SIMULATION_TURN_ADDR : RECOMP_GLOBAL_COMMAND_FRAME_ADDR);
```

`host/smoke_main.cpp` (add the include):

```cpp
const uint32_t kEntityBase = RECOMP_GLOBAL_ENTITY_BASE_ADDR;
const uint32_t kEntityStride = RECOMP_GLOBAL_ENTITY_BASE_STRIDE;
const uint32_t kEntityCount = RECOMP_GLOBAL_ENTITY_BASE_COUNT; // record 0 is the null entity
...
            uint32_t camera = rd32(RECOMP_HOOK_CAMERA);
```

- [ ] **Step 4: Extend the guard to these addresses**

In `tools/check_game_literals.py`, extend `TOKENS` with the exact literals that just left the code, so they cannot return:

```python
TOKENS = ("D3DPopTB", "C:\\\\Populous", "PopRecomp", '"Populous', "original/gog",
          "0x4a45a3", "0x4a47c1", "0x4a47a4", "0x98e7cc", "0x98e7e0",
          "0x005d5718", "0x005d571c", "0x00591b6c", "0x591b6c", "0xd0595c", "0xd0599c",
          "0x0089d188", "0x0089d184", "0x8e0428", "0x74a350")
```

Run: `.venv/bin/python -m pytest -q tests/test_game_literals.py`
Expected: pass. A failure names a line still carrying one of these values; convert it to the matching macro.

- [ ] **Step 5: Build, run the game-backed suites, commit**

Run: `.venv/bin/python tools/build.py --jobs 8 && .venv/bin/python tools/test.py --native`
Expected: pass.

Run: `.venv/bin/python tools/build.py --target smoke && .venv/bin/python tools/test.py --gameplay`
Expected: "all expectations met" in the smoke log and the three display modes reached.

```bash
.venv/bin/python tools/format.py --write
git add -A
git commit -m "Move frame clock, cursor, mouse and smoke addresses into games/populous/game.toml"
```

---

### Task 6: The translator and the oracle read the game config

**Files:**
- Create: `tools/recomp/tests/test_translate_config.py`
- Modify: `tools/recomp/translate.py`, `tools/recomp/oracle.py`, `tools/recomp/tests/test_translate.py`, `tools/build.py`, `tools/test.py`
- Test: `tools/recomp/tests/test_translate_config.py`, byte-identical regeneration against `build/baseline/gen`

**Interfaces:**
- Produces: `translate.configure(cfg)` setting module globals `LISTINGS`, `FUNCS_TSV`, `BINARY`, `CURATED`, `ANIMATION_COUNTER`, `VISUAL_ANIMATION_READS`; `translate.py --game <dir>`; `oracle.py --game <dir>`; `test_translate.py --game <dir>`.

- [ ] **Step 1: Write the failing test**

Create `tools/recomp/tests/test_translate_config.py`:

```python
"""translate.py takes its inputs from games/<id>/game.toml."""

import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
import game_config  # noqa: E402

spec = importlib.util.spec_from_file_location("translate", ROOT / "tools/recomp/translate.py")
translate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(translate)


class ConfigureTests(unittest.TestCase):
    def test_configure_sets_paths_and_volatile_reads(self):
        cfg = game_config.load(ROOT / "games/populous")
        translate.configure(cfg)
        self.assertEqual(Path(translate.LISTINGS), ROOT / "analysis/decompiled/D3DPopTB.exe/functions")
        self.assertEqual(Path(translate.FUNCS_TSV), ROOT / "analysis/decompiled/D3DPopTB.exe/functions.tsv")
        self.assertEqual(Path(translate.BINARY), ROOT / "original/gog/D3DPopTB.exe")
        self.assertEqual(Path(translate.CURATED), ROOT / "games/populous/globals.toml")
        self.assertEqual(translate.ANIMATION_COUNTER, 0x897981)
        self.assertEqual(len(translate.VISUAL_ANIMATION_READS), 16)
        self.assertIn(0x468F27, translate.VISUAL_ANIMATION_READS)

    def test_visual_animation_read_rewrites_the_configured_counter(self):
        cfg = game_config.load(ROOT / "games/populous")
        cfg["translate"]["animation_counter"] = 0x1234
        cfg["translate"]["volatile_reads"] = [0x10]
        translate.configure(cfg)
        body = ["c->r[0] = rd32(0x1234u);"]
        out = translate.visual_animation_read(0x10, body)
        self.assertEqual(out, ["c->r[0] = ((uint32_t)recomp_visual_animation_tick(rd32(0x1234u)));"])
```

Run: `.venv/bin/python -m pytest -q tools/recomp/tests/test_translate_config.py`
Expected: FAIL, `translate` has no attribute `configure`.

- [ ] **Step 2: Add `configure` to `translate.py`**

Replace the module-level constants and `VISUAL_ANIMATION_READS` block (lines 29 to 45 as imported) with:

```python
sys.path.insert(0, os.path.join(ROOT, "tools"))
import game_config  # noqa: E402

DEFAULT_GAME_DIR = os.path.join(ROOT, "games/populous")

# Set by configure(); module import configures the default game so the
# existing tests and tools that import this module keep their behaviour.
LISTINGS = FUNCS_TSV = BINARY = CURATED = None
ANIMATION_COUNTER = 0
VISUAL_ANIMATION_READS = frozenset()


def configure(cfg):
    """Point the translator at one game's listings, binary and audited reads."""
    global LISTINGS, FUNCS_TSV, BINARY, CURATED, ANIMATION_COUNTER, VISUAL_ANIMATION_READS
    listings = os.path.join(ROOT, cfg["translate"]["listings"])
    LISTINGS = os.path.join(listings, "functions")
    FUNCS_TSV = os.path.join(listings, "functions.tsv")
    BINARY = os.path.join(ROOT, cfg["game"]["developer_exe"])
    CURATED = os.path.join(str(cfg["dir"]), cfg["translate"].get("globals", "globals.toml"))
    ANIMATION_COUNTER = cfg["translate"]["animation_counter"]
    VISUAL_ANIMATION_READS = frozenset(cfg["translate"].get("volatile_reads", ()))


configure(game_config.load(DEFAULT_GAME_DIR))
```

Rewrite `visual_animation_read` to use the configured counter:

```python
def visual_animation_read(addr, body):
    if addr not in VISUAL_ANIMATION_READS:
        return body
    found = 0
    result = []
    pattern = r"rd(8|32)\(0x%xu\)" % ANIMATION_COUNTER
    replacement = "((uint%%s_t)recomp_visual_animation_tick(rd32(0x%xu)))" % ANIMATION_COUNTER
    for line in body:
        line, count = re.subn(pattern, lambda m: replacement % m[1], line)
        found += count
        result.append(line)
    if found != 1:
        raise TranslateError("visual animation read %08x no longer matches its audited operand" % addr)
    return result
```

In `main()`: add `ap.add_argument("--game", default=DEFAULT_GAME_DIR, help="games/<id> directory")` and, first thing after parsing, `configure(game_config.load(args.game))`. Replace `os.path.join(ROOT, "games/populous/globals.toml")` (line 2362 after Task 2) with `CURATED`. Update the module docstring's first paragraph to say inputs come from `games/<id>/game.toml`.

Run `grep -n 'LISTINGS\|FUNCS_TSV\|BINARY\b' tools/recomp/translate.py | head` and confirm every use reads the module global at call time (none captured at definition time in a default argument). If one is a default argument, change it to `None` with `if x is None: x = LISTINGS` inside the function.

- [ ] **Step 3: Oracle, differential test, build script**

`tools/recomp/oracle.py`: replace

```python
DEFAULT_EXE = os.path.join(ROOT, "original/gog/D3DPopTB.exe")
DEFAULT_DATA = os.path.join(ROOT, "original/gog")
```

with

```python
sys.path.insert(0, os.path.join(ROOT, "tools"))
import game_config  # noqa: E402
DEFAULT_GAME_DIR = os.path.join(ROOT, "games/populous")
_cfg = game_config.load(DEFAULT_GAME_DIR)
DEFAULT_EXE = os.path.join(ROOT, _cfg["game"]["developer_exe"])
DEFAULT_DATA = os.path.dirname(DEFAULT_EXE)
```

and add `ap.add_argument("--game", default=DEFAULT_GAME_DIR)`; when `--game` differs from the default, recompute `args.exe` and `args.data` from that config unless they were given explicitly.

`tools/recomp/tests/test_translate.py`: replace `BINARY = os.path.join(ROOT, "original/gog/D3DPopTB.exe")` with the same three lines (`game_config` import, `_cfg`, `BINARY = os.path.join(ROOT, _cfg["game"]["developer_exe"])`).

`tools/build.py` `run_translator`: pass `"--game", str(ROOT / "games" / game)`; thread `game` through from `main` (make `run_translator` a closure or `functools.partial` with `args.game`).

- [ ] **Step 4: Run the new test and the portable suites**

Run: `.venv/bin/python -m pytest -q tools/recomp/tests/test_translate_config.py tools/recomp/tests/test_translate_hooks.py`
Expected: pass. Add `"tools/recomp/tests/test_translate_config.py"` to `PORTABLE_TESTS` in `tools/test.py` and run `.venv/bin/python tools/test.py`.

- [ ] **Step 5: Prove regeneration is byte-identical**

Run: `.venv/bin/python tools/build.py --regenerate --target gen`
Then:

```bash
diff -rq build/baseline/gen build/recomp/gen && echo IDENTICAL
```

Expected: `IDENTICAL`. A difference means `configure` changed an input; inspect the first differing chunk with `diff` and fix before continuing.

- [ ] **Step 6: Commit**

```bash
.venv/bin/python tools/format.py
git add -A
git commit -m "Translator, oracle and differential test read games/<id>/game.toml"
```

---

### Task 7: Stub translation so every host links without game code

**Files:**
- Create: `tools/gen_stub_translation.py`, `tests/test_gen_stub_translation.py`
- Modify: `cmake/Translate.cmake`, `tools/build.py`, `tools/test.py`
- Test: `tests/test_gen_stub_translation.py`, a configure with `-DPOP_TRANSLATE=STUB`

**Interfaces:**
- Produces: `POP_TRANSLATE=STUB` configure mode; `tools/gen_stub_translation.py --out DIR` writing `table.c`, `funcs.h`, `x86.h`; `tools/build.py --stub`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_gen_stub_translation.py`:

```python
"""The stub translation defines every symbol the real table.c exports."""

import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("gen_stub_translation", ROOT / "tools/gen_stub_translation.py")
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)

EXPORTS = ("recomp_func_addrs", "recomp_func_count", "recomp_profile_name", "recomp_base_ptrs",
           "recomp_raw_ptrs", "recomp_hooked", "recomp_override_count", "recomp_override_hash",
           "recomp_lookup", "recomp_index_of", "recomp_call", "recomp_jump", "recomp_unknown_jump")


class StubTests(unittest.TestCase):
    def test_writes_every_export_and_compiles(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            gen.write(out)
            table = (out / "table.c").read_text()
            for name in EXPORTS:
                self.assertIn(name, table)
            self.assertTrue((out / "funcs.h").is_file())
            self.assertTrue((out / "x86.h").is_file())
            subprocess.run(["clang", "-std=c11", "-fsyntax-only", "-Wall", "-Wextra",
                            "-I", str(out), "-I", str(ROOT / "runtime"), str(out / "table.c")], check=True)
```

Run: `.venv/bin/python -m pytest -q tests/test_gen_stub_translation.py`
Expected: FAIL, no such file.

- [ ] **Step 2: Write `tools/gen_stub_translation.py`**

Before writing, list the real exports so the stub matches them exactly:

```bash
grep -nE '^(const |void |int32_t |uint32_t |uint64_t |uint8_t |const char \*)[a-z_ ]*recomp_[a-z_]+' \
  <workspace>/populous-recomp-checkout/translation/table.c | cut -c1-120
```

Then:

```python
#!/usr/bin/env python3
"""A link-only translation: every symbol build/recomp/gen/table.c exports, with
zero functions. CI links the hosts with it because generated code is never
tracked and the game is never available there. A host running on it fails at
the first guest call with a clear message; it exists to be linked, not run.

    tools/gen_stub_translation.py --out DIR
"""

import argparse
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]

FUNCS_H = """/* generated by tools/gen_stub_translation.py -- link-only stub, do not edit */
#ifndef RECOMP_FUNCS_H
#define RECOMP_FUNCS_H
#include "x86.h"
#include "intrinsics.h"
#endif
"""

TABLE_C = """/* generated by tools/gen_stub_translation.py -- link-only stub, do not edit */
#include <stdio.h>
#include <stdint.h>
#include "funcs.h"

const uint32_t recomp_func_addrs[1] = {0};
const uint32_t recomp_func_count = 0;
const char *recomp_profile_name(uint32_t i) { (void)i; return 0; }
void (*const recomp_base_ptrs[1])(X86 *) = {0};
void (*const recomp_raw_ptrs[1])(X86 *) = {0};
uint8_t recomp_hooked[1];
uint32_t recomp_override_count(void) { return 0; }
uint64_t recomp_override_hash(void) { return 0; }
int32_t recomp_lookup(uint32_t addr) { (void)addr; return -1; }
int32_t recomp_index_of(uint32_t addr) { return recomp_lookup(addr); }

static void stub_trap(X86 *c, const char *what, uint32_t target)
{
    fprintf(stderr, "[stub translation] %s to %08x: this build has no generated code\\n", what, target);
    c->r[R_EAX] = 0;
}
void recomp_call(X86 *c, uint32_t target) { stub_trap(c, "call", target); }
void recomp_jump(X86 *c, uint32_t target) { stub_trap(c, "jump", target); }
void recomp_unknown_jump(X86 *c, uint32_t target) { stub_trap(c, "unknown jump", target); }
"""


def write(out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "funcs.h").write_text(FUNCS_H)
    (out / "table.c").write_text(TABLE_C)
    shutil.copy(ROOT / "runtime/x86.h", out / "x86.h")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    write(parser.parse_args().out)


if __name__ == "__main__":
    main()
```

If the export listing above shows a declaration whose type differs from the stub (for example `recomp_lookup` declared `static`), match the real one: the goal is that every host links. Run the test:

Run: `.venv/bin/python -m pytest -q tests/test_gen_stub_translation.py`
Expected: pass. Add the file to `PORTABLE_TESTS` in `tools/test.py`.

- [ ] **Step 3: Teach `cmake/Translate.cmake` the STUB mode**

Change the cache property line to `set_property(CACHE POP_TRANSLATE PROPERTY STRINGS AUTO ON OFF STUB)` and insert, before `set(POP_HAVE_GEN OFF)`:

```cmake
if(POP_TRANSLATE STREQUAL "STUB")
  # A link-only translation for builds without game code (CI).
  set(POP_GEN_DIR ${CMAKE_BINARY_DIR}/stub-gen)
  execute_process(
    COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/tools/gen_stub_translation.py --out ${POP_GEN_DIR}
    RESULT_VARIABLE POP_STUB_RESULT)
  if(NOT POP_STUB_RESULT EQUAL 0)
    message(FATAL_ERROR "tools/gen_stub_translation.py failed")
  endif()
endif()
```

and make the following chain read:

```cmake
set(POP_HAVE_GEN OFF)
if(POP_TRANSLATE STREQUAL "OFF")
  message(STATUS "POP_TRANSLATE=OFF: targets that need the generated code are not defined")
elseif(EXISTS ${POP_GEN_DIR}/table.c)
  set(POP_HAVE_GEN ON)
  message(STATUS "Translation: ${POP_GEN_DIR}")
elseif(POP_TRANSLATE STREQUAL "ON")
  message(FATAL_ERROR "POP_TRANSLATE=ON but no translation: run tools/build.py --regenerate")
else()
  message(STATUS "No translation found: hosts and game-backed tests are not defined")
endif()
```

- [ ] **Step 4: Expose it in `tools/build.py`**

Stub builds get their own presets so they never shadow a real build directory and CI can use them without Python. Add to `CMakePresets.json`, in `configurePresets`:

```json
    {"name": "macos-stub", "inherits": "macos", "cacheVariables": {"POP_TRANSLATE": "STUB"}},
    {"name": "linux-stub", "inherits": "linux", "cacheVariables": {"POP_TRANSLATE": "STUB"}},
    {"name": "windows-stub", "inherits": "windows", "cacheVariables": {"POP_TRANSLATE": "STUB"}}
```

in `buildPresets`:

```json
    {"name": "macos-stub", "configurePreset": "macos-stub"},
    {"name": "linux-stub", "configurePreset": "linux-stub"},
    {"name": "windows-stub", "configurePreset": "windows-stub"}
```

and in `testPresets`:

```json
    {"name": "macos-stub", "configurePreset": "macos-stub", "output": {"outputOnFailure": true}},
    {"name": "linux-stub", "configurePreset": "linux-stub", "output": {"outputOnFailure": true}},
    {"name": "windows-stub", "configurePreset": "windows-stub", "output": {"outputOnFailure": true}}
```

In `tools/build.py`: add `parser.add_argument("--stub", action="store_true", help="Link the hosts against a stub translation (no game code)")`; make `preset_name(preset, config, stub=False)` return `preset + "-stub"` when `stub` is true; reject `--stub --config Debug` and `--stub --regenerate` with `parser.error`; skip the `developer_exe` and listings checks when `--stub` is given; pass `stub=args.stub` from `main`.

Add to `tests/test_build_py.py`:

```python
    def test_stub_selects_the_stub_preset(self):
        args, _ = build_py.parse_args(["--stub"], system="Linux")
        self.assertEqual(build_py.preset_name(args.preset, args.config, stub=args.stub), "linux-stub")
```

and give `preset_name(preset, config, stub=False)` that behaviour (`-stub` wins over `-debug`; a stub debug build is not supported and `parse_args` rejects `--stub --config Debug`).

- [ ] **Step 5: Verify locally**

Run: `.venv/bin/python tools/test.py`
Expected: pass.

Run: `.venv/bin/python tools/build.py --stub --jobs 8 && .venv/bin/python tools/build.py --stub --target headless && .venv/bin/python tools/build.py --stub --target smoke`
Expected: `recomp_app`, `pop_headless` and `pop_smoke` link under `build/cmake/macos-stub/`. Note the app bundle in `build/PopRecomp.app` is now the stub one; rebuild normally afterwards with `.venv/bin/python tools/build.py --jobs 8` and confirm it launches.

Run: `ctest --preset macos-stub -L nogame --output-on-failure`
Expected: pass.

- [ ] **Step 6: Commit**

```bash
.venv/bin/python tools/format.py
git add -A
git commit -m "Add a link-only stub translation and stub presets for builds without game code"
```

---

### Task 8: CI, documentation, spec amendment

**Files:**
- Modify: `.github/workflows/checks.yml`, `README.md`, `CONTRIBUTING.md`, `AGENTS.md`, `CHANGELOG.md`, `docs/superpowers/specs/2026-09-13-recomp-kit-design.md`, `Makefile`
- Test: `tools/check_repo.py`, the workflow run on the private remote

- [ ] **Step 1: Rewrite the CI workflow**

In `.github/workflows/checks.yml`:
- rename the workflow to `Checks`;
- in the `native-compile` matrix, change each `preset` to its `-stub` variant (`macos-stub`, `linux-stub`, `windows-stub`) and update the comment to say the hosts link against the stub translation because generated code is never tracked;
- replace the step `Build the hosts from the tracked translation` with:

```yaml
      - name: Link every host against the stub translation
        run: cmake --build --preset ${{ matrix.preset }} --target recomp_app pop_headless pop_smoke
```

- delete the `Package the hosts as a downloadable archive` step and the `actions/upload-artifact` step entirely;
- in the `tooling` job, keep `tools/test.py`, `tools/format.py`, `tools/check_repo.py` as they are.

- [ ] **Step 2: Rewrite the top-level documents for the kit**

`README.md` becomes:

```markdown
# recomp-kit

A static recompilation kit: 32-bit x86 Windows games become native
applications for macOS, iOS, Android, Linux and Windows, with no JIT and no
emulator at run time. The design is in
`docs/superpowers/specs/2026-09-13-recomp-kit-design.md`; this milestone
(M0) consolidates the Populous: The Beginning recompilation as the first
supported game.

## Layout

| Directory | What it holds |
|---|---|
| `runtime/` | x86 semantics (`x86.h`), guest memory, PE loader, scheduler, kernel32/user32 shims |
| `dx/` | DirectDraw, Direct3D 2, DirectSound, DirectInput, QMixer shims |
| `host/` | SDL3 host, Metal/Vulkan/fake GPU backends, audio mixer, presentation |
| `platform/` | `os.h`, the only place that talks to the operating system |
| `mods/` | the mod foundation (Lua 5.4) and its native capture instruments |
| `games/<id>/` | one game: `game.toml`, `globals.toml`, plugins, artwork. No game bytes |
| `tools/` | translator, oracle, build and test scripts |
| `third_party/` | vendored Lua, TinySoundFont, volk, Vulkan headers |

## Build Populous on macOS

You need your own DRM-free `D3DPopTB.exe` with its data, Ghidra for the
listings, and Python 3.9 or later.

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements-dev.txt
.venv/bin/python tools/setup.py --game-dir /path/to/your/populous --ghidra-home /path/to/ghidra
.venv/bin/python tools/build.py --regenerate
open build/PopRecomp.app
```

Generated code is never tracked. A build without game files links the hosts
against a stub translation: `.venv/bin/python tools/build.py --stub`.

## Check a change

```sh
.venv/bin/python tools/test.py            # portable Python suites
.venv/bin/python tools/format.py          # handwritten native code style
.venv/bin/python tools/check_repo.py      # nothing private is tracked
.venv/bin/python tools/check_game_literals.py   # kit code names no game
.venv/bin/python tools/test.py --native   # native suites (needs the game)
```

Every game-specific value lives in `games/<id>/`; the kit's own directories
must not name a game. `tests/test_game_literals.py` enforces that.
```

`CONTRIBUTING.md`: replace `src/recomp/` mentions (Task 2 did), replace the sentence "Building the app no longer requires this step: `translation/` is tracked." with "Building the app requires this step; generated code is never tracked.", and add a section:

```markdown
## Add a game

Create `games/<id>/game.toml` and `games/<id>/globals.toml` following
`games/populous/`. Build with `tools/build.py --game <id>`. Nothing under
`runtime/`, `dx/`, `host/` or `platform/` may name your game; put addresses
under `[hooks]` and use the generated `RECOMP_HOOK_*` macros.
```

`AGENTS.md`: change the title to "Working on recomp-kit", replace `platform/os.h` path text as needed, and add the bullet: "Every game-specific literal belongs in `games/<id>/`; run `tools/check_game_literals.py` before committing native code."

`Makefile`: add a `stub` target running `$(PYTHON) tools/build.py --stub` and list it in `help`.

- [ ] **Step 3: Amend the spec**

In `docs/superpowers/specs/2026-09-13-recomp-kit-design.md`, section 4's layout block: add the line `  platform/   os.h seam with POSIX and Win32 implementations; runtime/ links it` after `host/`, and add after the block:

```markdown
M0 note: `win32/` and the `lift/`/`verify/` split of the Python tooling are
deferred to M2, when the auto-stub generator and the `recomp` CLI are written.
`mods/` is carried as an opaque component because every host links it.
`tools/setup.py`, the smoke scripts, `tools/recomp/package.py` and the
texture-pack tooling remain Populous-specific until M2 and M4 respectively.
```

- [ ] **Step 4: Changelog and commit**

Add under `## Unreleased` in `CHANGELOG.md`:

```
- Kit layout: runtime/, dx/, host/, platform/, mods/ at the top level; games/populous/
  holds the game config, curated globals, plugins and artwork.
- games/<id>/game.toml drives identity, addresses and translator inputs through a
  generated game_config.h; tools/check_game_literals.py keeps game literals out of kit code.
- tools/build.py --stub and the *-stub CMake presets link the hosts without game code.
```

```bash
.venv/bin/python tools/check_repo.py
.venv/bin/python tools/test.py
git add -A
git commit -m "CI links hosts against the stub translation; kit README, contributing and spec amendment"
```

- [ ] **Step 5: Push to a private remote and watch CI**

This needs `gh auth status` to succeed. If it does:

```bash
gh repo create recomp-kit --private --source . --remote origin --push
gh run watch --exit-status
```

Expected: the `tooling` job and all three `native-compile` jobs pass. If `gh` is not authenticated, stop here and report that CI on Linux and Windows is unverified.

---

### Task 9: Acceptance run for M0

**Files:**
- No source changes expected. `CHANGELOG.md` only if a fix was needed.
- Test: everything.

- [ ] **Step 1: Clean regeneration and build**

```bash
cd <workspace>/recomp-kit
rm -rf build/cmake build/recomp/gen
.venv/bin/python tools/build.py --regenerate --jobs 8
diff -rq build/baseline/gen build/recomp/gen && echo IDENTICAL
```

Expected: `IDENTICAL` and `build/PopRecomp.app` present.

- [ ] **Step 2: Every suite**

```bash
.venv/bin/python tools/test.py
.venv/bin/python tools/format.py
.venv/bin/python tools/check_repo.py
.venv/bin/python tools/check_game_literals.py
.venv/bin/python tools/test.py --native
.venv/bin/python tools/test.py --mods
.venv/bin/python tools/build.py --target smoke && .venv/bin/python tools/test.py --gameplay
.venv/bin/python tools/recomp/tests/test_translate.py 2>&1 | tail -3 | tee build/baseline/test_translate.after.txt
diff build/baseline/test_translate.txt build/baseline/test_translate.after.txt && echo "differential unchanged"
.venv/bin/python tools/recomp/oracle.py --frames 32 --out build/recomp/parity/oracle
```

Expected: every command exits 0; the differential summary matches the Task 1 baseline; the oracle run completes 32 frames.

- [ ] **Step 3: Play**

`open build/PopRecomp.app`: start a level, select and move units, open Options with F10, change the resolution, quit with Command-Q. Confirm the settings persisted on relaunch.

- [ ] **Step 4: Tag**

```bash
git tag -a m0 -m "M0: Populous builds from games/populous/ in recomp-kit"
git push origin main --tags   # only if Task 8 Step 5 created the remote
```

Report exactly which checks ran and their results. Compilation and offscreen counters do not establish playable performance; the play check in Step 3 does.
