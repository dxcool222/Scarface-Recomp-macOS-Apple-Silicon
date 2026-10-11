# Game Repo Split Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make recomp-kit build any game from a directory outside the kit, give the kit a stub game for its own CI, move everything Populous-only into populous-recomp, and turn populous-recomp into a thin repo (game folder + kit submodule + wrappers) that installs the iOS port from its own checkout.

**Architecture:** Two CMake cache paths (`RECOMP_GAME_DIR`, `POP_BUILD_ROOT`) replace the assumption that the game and the build live under the kit root; `tools/build.py --game-dir` sets both and every tool derives its paths from them. `games/stub` keeps the kit self-sufficient. populous-recomp's tree is replaced by the moved game folder, with four-line wrappers that exec the kit's tools.

**Tech Stack:** CMake 3.24 presets, Python 3.9 tooling (`tools/*.py`), git submodules, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-09-13-game-repo-split-design.md`

## Global Constraints

- `RECOMP_GAME_DIR` is an absolute path holding `game.toml`; default `${POP_ROOT}/games/stub`. `POP_BUILD_ROOT` defaults to `${POP_ROOT}/build`; for a game directory outside the kit it is `<game-dir>/build` (spec 5.1, 5.2).
- `developer_exe` and `translate.listings` resolve relative to the directory holding `game.toml` (spec 5.3).
- The stub game is `games/stub/` with the exact `game.toml` of spec 5.4 and a `globals.toml` declaring `simulation_turn`, `command_frame` and `entity_base` (stride 179, count 2000) at distinct 0x0070xxxx addresses.
- Every Populous-only file listed in spec section 6 leaves the kit in Task 6; nothing else about Populous changes (comments stay; M2 generalises).
- Commands run from the repo root through `tools/build.py` / `tools/test.py`; format with `.venv/bin/python tools/format.py --write` before each commit; never chain `build.py | grep`.
- Kit work on branch `game-repo-split` (from `main` at `m1`); game repo work on branch `thin-kit` in `<workspace>/populous-recomp-checkout`.
- Deleting or replacing trees in populous-recomp happens only after tag `legacy-macos-source` exists and is pushed (Task 7 step 1).

---

### Task 1: `games/stub`

**Files:**
- Create: `games/stub/game.toml`, `games/stub/globals.toml`
- Modify: `tests/test_game_literals.py` (the stub carries no Populous value), `tools/check_game_literals.py` (scan `games/stub`)

- [ ] **Step 1: Write the failing test**

Add to `tests/test_game_literals.py` (follow its existing style; it invokes `check_game_literals.py` on a tree):

```python
    def test_the_stub_game_has_no_populous_literal(self):
        stub = ROOT / "games/stub"
        self.assertTrue((stub / "game.toml").is_file(), "games/stub/game.toml is missing")
        text = (stub / "game.toml").read_text() + (stub / "globals.toml").read_text()
        for token in ("Populous", "D3DPopTB", "PopRecomp", "0x0055d6c0", "0x00d0595c"):
            self.assertNotIn(token, text)
```

Run: `.venv/bin/python -m pytest -q tests/test_game_literals.py > /tmp/t.log 2>&1; echo exit $?; tail -3 /tmp/t.log`
Expected: fails on the missing file.

- [ ] **Step 2: Write the stub**

`games/stub/game.toml` is spec section 5.4 verbatim, with this header comment:

```toml
# games/stub/game.toml - a game that does not exist. Every RECOMP_* macro the
# kit compiles against gets a value from here, so the link-only *-stub presets,
# the iOS stub compile and the game-free unit tests configure without a real
# game. No test asserts anything about these numbers. Real games live in their
# own repositories and are built with tools/build.py --game-dir.
```

`games/stub/globals.toml`:

```toml
# games/stub/globals.toml - the curated globals the kit's code reads, at
# addresses that mean nothing. See games/stub/game.toml.
[globals.simulation_turn]
addr = 0x00700000
size = 4

[globals.command_frame]
addr = 0x00700010
size = 4

[globals.entity_base]
addr = 0x00701000
stride = 179
count = 2000
```

Check the field names against Populous's `globals.toml` (`sed -n 10,45p games/populous/globals.toml`) and against `tools/gen_game_config.py`'s `("size", "stride", "count")`; copy any other key the translator requires (`grep -n 'globals\[' tools/recomp/translate.py | head`).

- [ ] **Step 3: Configure the stub build on the stub game**

Run: `.venv/bin/python tools/build.py --stub --game stub --jobs 8 > /tmp/b.log 2>&1; echo exit $?` (the `--game NAME` form still exists at this point)
Expected: exit 0; `grep -c 'STUB.EXE' build/cmake/macos-stub/generated/game_config.h` prints 1. If the translator's stub generator or a `RECOMP_GLOBAL_*` consumer complains about a missing global, add it to `globals.toml`.

- [ ] **Step 4: Run the literal test and commit**

Run: `.venv/bin/python -m pytest -q tests/test_game_literals.py > /tmp/t.log 2>&1; echo exit $?`
Expected: exit 0.

```bash
git add games/stub tests/test_game_literals.py tools/check_game_literals.py
git commit -m "games/stub: a fake game so the kit configures without a real one"
```

---

### Task 2: `RECOMP_GAME_DIR` and `POP_BUILD_ROOT` in CMake

**Files:**
- Modify: `CMakeLists.txt:23-36,45-69,110-113`, `cmake/Translate.cmake:4`, `cmake/IosBundle.cmake:21-38,46-51`, `cmake/MacBundle.cmake` (finish_bundle arguments), `mods/CMakeLists.txt:80-92` (roots tests out), `games/populous/mods/CMakeLists.txt` (roots tests in), `runtime/CMakeLists.txt:37`, `host/CMakeLists.txt` (`present_events_tests`, `game_view_tests` guards), `mods/CMakeLists.txt` (`game_view_tests` include path)
- Modify: `tools/gen_game_config.py:57-63` (absolute developer paths), `tools/game_config.py` (resolved paths), `tests/test_game_config.py`

**Interfaces:**
- Produces: CMake cache `RECOMP_GAME_DIR` (PATH), `POP_BUILD_ROOT` (PATH); generated cmake `RECOMP_DEVELOPER_GAME_DIR`/`RECOMP_DEVELOPER_EXE` absolute; Python `cfg["developer_exe_path"]`, `cfg["listings_path"]` (absolute `Path`).

- [ ] **Step 1: Failing Python tests for path resolution**

In `tests/test_game_config.py` add:

```python
    def test_developer_paths_resolve_relative_to_the_game_dir(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp) / "g"
            game.mkdir()
            (game / "game.toml").write_text((ROOT / "games/stub/game.toml").read_text())
            (game / "globals.toml").write_text((ROOT / "games/stub/globals.toml").read_text())
            cfg = game_config.load(game)
            self.assertEqual(cfg["developer_exe_path"], (game / "original/STUB.EXE").resolve())
            self.assertEqual(cfg["listings_path"], (game / "analysis/STUB.EXE").resolve())
            cmake = gen_game_config.render_cmake(cfg)
            self.assertIn('set(RECOMP_DEVELOPER_EXE "%s")' % (game / "original/STUB.EXE").resolve(), cmake)
            self.assertIn('set(RECOMP_DEVELOPER_GAME_DIR "%s")' % (game / "original").resolve(), cmake)
```

Run: `.venv/bin/python -m pytest -q tests/test_game_config.py > /tmp/t.log 2>&1; echo exit $?` → fails (`KeyError: developer_exe_path`).

- [ ] **Step 2: Resolve the paths**

In `tools/game_config.py` `load()`, before `return cfg`:

```python
    cfg["developer_exe_path"] = (game_dir / game["developer_exe"]).resolve()
    cfg["listings_path"] = (game_dir / translate.get("listings", "analysis")).resolve()
```

In `tools/gen_game_config.py` `render_cmake` replace the two developer lines with:

```python
    lines.append('set(RECOMP_DEVELOPER_GAME_DIR "%s")' % cfg["developer_exe_path"].parent.as_posix())
    lines.append('set(RECOMP_DEVELOPER_EXE "%s")' % cfg["developer_exe_path"].as_posix())
```

Update `test_cmake_fragment` in `tests/test_game_config.py` to expect absolute paths (`(ROOT / "games/populous/original/gog").resolve()`; note `original` is a symlink at the kit root today, not under `games/populous`, so this assertion will read `ROOT/games/populous/original/gog` — that is the intended new meaning; Task 6 moves the game and Task 7 gives it a real `original/`). Run the tests: exit 0.

- [ ] **Step 3: CMake cache paths**

In `CMakeLists.txt` replace lines 23-36 with:

```cmake
set(POP_ROOT ${CMAKE_SOURCE_DIR})
set(POP_TRANSLATE AUTO CACHE STRING "AUTO, ON, OFF or STUB: how targets that need generated code get it")
set_property(CACHE POP_TRANSLATE PROPERTY STRINGS AUTO ON OFF STUB)
# Where every output goes: the translation, the texture pack, the apps and the
# logs. A game outside the kit keeps them beside itself (tools/build.py passes
# <game-dir>/build); the kit's own builds use build/.
set(POP_BUILD_ROOT ${POP_ROOT}/build CACHE PATH "Root of build outputs (build/recomp, build/ios, ...)")
# A stub build (no game code) keeps its outputs apart from a real build so it
# can never overwrite the archive or the bundle a player runs.
if(IOS AND POP_TRANSLATE STREQUAL "STUB")
  set(POP_BUILD_DIR ${POP_BUILD_ROOT}/ios-stub)
elseif(POP_TRANSLATE STREQUAL "STUB")
  set(POP_BUILD_DIR ${POP_BUILD_ROOT}/stub)
elseif(IOS)
  set(POP_BUILD_DIR ${POP_BUILD_ROOT}/ios)
else()
  set(POP_BUILD_DIR ${POP_BUILD_ROOT})
endif()
```

Replace lines 45-52 with:

```cmake
# The game this build is for: a directory holding game.toml, rendered into
# generated/game_config.h (C macros) and generated/game_config.cmake (build
# names) at configure time; editing the toml reconfigures. The kit's own
# default is the stub game; real games live in their own repositories.
set(RECOMP_GAME_DIR ${POP_ROOT}/games/stub CACHE PATH "Directory holding the game.toml this build is for")
if(NOT IS_ABSOLUTE ${RECOMP_GAME_DIR})
  message(FATAL_ERROR "RECOMP_GAME_DIR must be absolute: ${RECOMP_GAME_DIR}")
endif()
if(NOT EXISTS ${RECOMP_GAME_DIR}/game.toml)
  message(FATAL_ERROR "No game config at ${RECOMP_GAME_DIR}/game.toml")
endif()
```

Replace lines 110-113 with:

```cmake
# The game's own mods (core plugins, examples, smoke probe), when it has any.
if(NOT IOS AND EXISTS ${RECOMP_GAME_DIR}/mods/CMakeLists.txt)
  add_subdirectory(${RECOMP_GAME_DIR}/mods ${CMAKE_BINARY_DIR}/game-mods)
endif()
```

In `cmake/Translate.cmake` line 4: `set(POP_GEN_DIR ${POP_BUILD_ROOT}/recomp/gen)`. In `cmake/IosBundle.cmake`: `--source ${RECOMP_DEVELOPER_GAME_DIR}` and `--exe ${RECOMP_DEVELOPER_EXE}` (no `${POP_ROOT}/` prefix, they are absolute now); `${POP_BUILD_ROOT}/recomp/symbols.json` in both the `EXISTS` and the copy. In `cmake/MacBundle.cmake` pass `--build-root ${POP_BUILD_ROOT}` to `finish_bundle.py` and add that argument to `tools/recomp/finish_bundle.py` (default `ROOT / "build"`), using it for the texture pack default and `symbols.json`.

- [ ] **Step 4: Move the roots tests into the game's mods**

Cut the `roots_probe`/`roots_tests` block (`mods/CMakeLists.txt` lines 80-92) and paste it into `games/populous/mods/CMakeLists.txt` with paths `${CMAKE_CURRENT_SOURCE_DIR}/core/tests/roots_probe.cpp` and `.../roots_tests.py`, and `${POP_ROOT}/mods/roots.cpp` for the kit source; keep `target_include_directories(roots_probe PRIVATE ${POP_ROOT}/mods)`.

- [ ] **Step 5: Guard the Populous-fixture tests**

- `runtime/CMakeLists.txt:37`: `add_test(NAME runtime_tests COMMAND runtime_tests WORKING_DIRECTORY ${RECOMP_GAME_DIR})` and in `runtime/tests/runtime_tests.cpp` replace the three `"original/gog/D3DPopTB.exe"` literals with `RECOMP_DEVELOPER_EXE` (include `"game_config.h"`; the macro is an absolute path string after Step 2, so the working directory no longer matters for it) and guard the whole suite's game-backed section with `if (access(RECOMP_DEVELOPER_EXE, R_OK) != 0) { printf("runtime_tests: no game at %s; game-backed checks skipped\n", RECOMP_DEVELOPER_EXE); }` around the loader checks only (the registry/path-string checks that use `C:\Populous` literals compare against `RECOMP_GUEST_ROOT` instead: replace `"C:\\Populous"` with `RECOMP_GUEST_ROOT` and `"C:\\Populous\\D3DPopTB.exe"` with `RECOMP_GUEST_ROOT "\\" RECOMP_EXECUTABLE`).
- `mods/tests/game_view_tests.cpp:8`: `#include "tests/entity_codec.hpp"` with `target_include_directories(... ${RECOMP_GAME_DIR})`, and in `mods/CMakeLists.txt` wrap the target in `if(EXISTS ${RECOMP_GAME_DIR}/tests/entity_codec.hpp)`.
- `mods/tests/present_events_tests.mm:87-91`: `root / "games/populous/mods/examples/luawalk/..."` becomes `game_dir / "mods/examples/luawalk/..."` with `game_dir` read from `getenv("RECOMP_GAME_DIR")`; set `ENVIRONMENT RECOMP_GAME_DIR=${RECOMP_GAME_DIR}` on the test in `host/CMakeLists.txt:154` and wrap the target in `if(EXISTS ${RECOMP_GAME_DIR}/mods/examples/luawalk/main.lua)`.
- `mods/tests/fixtures/display_projection.c:2`: `#include "mods/core/display/display.c"` with `${RECOMP_GAME_DIR}` on the include path of the fixture target; guard with `if(EXISTS ${RECOMP_GAME_DIR}/mods/core/display/display.c)`.
- `host/tests/integration_tests.sh:18,186`: `"${RECOMP_GAME_DIR:?set RECOMP_GAME_DIR}/mods/core/tests/roots_tests.sh"` and `cmp "$RECOMP_GAME_DIR/mods/core/README.md" ...`.

- [ ] **Step 6: Configure and test both ways**

Run (still with the old `--game` flag, which Task 3 replaces; drive cmake directly here):

```bash
.venv/bin/cmake --preset macos-stub -DPython3_EXECUTABLE=$PWD/.venv/bin/python > /tmp/c1.log 2>&1; echo exit $?
.venv/bin/cmake --preset macos -DPython3_EXECUTABLE=$PWD/.venv/bin/python -DRECOMP_GAME_DIR=$PWD/games/populous > /tmp/c2.log 2>&1; echo exit $?
.venv/bin/cmake --build --preset macos --target check_binaries --parallel 8 > /tmp/b2.log 2>&1; echo exit $?
.venv/bin/ctest --preset macos -L 'nogame|gpu|game' --output-on-failure > /tmp/t2.log 2>&1; echo exit $?
```

Expected: all exit 0 (the `game` label runs because `games/populous/original` does not exist yet, so `runtime_tests` prints its skip line; create a temporary symlink `games/populous/original -> ../../original` to run them for real, and remove it after). Fix whatever names `RECOMP_GAME` (`grep -rn RECOMP_GAME CMakeLists.txt cmake tools tests | grep -v RECOMP_GAME_DIR`).

- [ ] **Step 7: Commit**

```bash
.venv/bin/python tools/format.py --write
git add -A
git commit -m "CMake: RECOMP_GAME_DIR and POP_BUILD_ROOT; game paths resolve from game.toml's directory"
```

---

### Task 3: `tools/build.py --game-dir` and the build root in every tool

**Files:**
- Modify: `tools/build.py`, `tools/test.py`, `tools/setup.py`, `tools/ios_logs.py`, `tools/recomp/buildlock.py`, `tools/recomp/translate.py:28-58`, `tools/recomp/oracle.py:44-49`, `tools/recomp/build_core.py:129-132`, `tools/recomp/finish_bundle.py`, `host/tests/integration_tests.sh`
- Test: `tests/test_build_py.py`, `tools/recomp/tests/test_translate_config.py`, `tools/recomp/tests/test_buildlock.py`

**Interfaces:**
- Produces in `tools/build.py`: `parse_args(argv, system)` accepting `--game-dir PATH` (default `ROOT/"games/stub"`) and `--build-root PATH`; `build_root_for(game_dir, root=ROOT)` returning `root/"build"` when `game_dir` is under `root`, else `game_dir/"build"`; `configure(preset, extra, build_dir)`; `archive_path(build_root, system)`; `ios_app_bundle(app_name, build_root)`; `publish_generated(build_root, kit_root, translate)`; `run_translator(stage, game_dir, build_root)`; `texture_pack(game_dir, build_root)`.

- [ ] **Step 1: Failing tests**

Replace `test_game_defaults_to_populous_and_is_validated` in `tests/test_build_py.py` with:

```python
    def test_game_dir_defaults_to_the_stub_and_is_validated(self):
        args, _ = build_py.parse_args([], system="Darwin")
        self.assertEqual(args.game_dir, build_py.ROOT / "games/stub")
        with self.assertRaises(SystemExit):
            build_py.parse_args(["--game-dir", "/no/such/game"], system="Darwin")
        with self.assertRaises(SystemExit):
            build_py.parse_args(["--game-dir", "games/stub"], system="Darwin")  # relative

    def test_build_root_follows_an_external_game_dir(self):
        self.assertEqual(build_py.build_root_for(build_py.ROOT / "games/stub"), build_py.ROOT / "build")
        self.assertEqual(build_py.build_root_for(Path("/tmp/populous-recomp")), Path("/tmp/populous-recomp/build"))

    def test_configure_passes_the_build_dir_and_the_cache_paths(self):
        with mock.patch("subprocess.run") as run:
            build_py.configure("macos", ["-DX=1"], build_dir=Path("/tmp/pr/build/cmake/macos"))
        command = run.call_args[0][0]
        self.assertIn("-B", command)
        self.assertEqual(command[command.index("-B") + 1], "/tmp/pr/build/cmake/macos")
```

Update `test_ios_target_uses_the_ios_preset_and_needs_macos` and any other test that passes `--game` to use `--game-dir` with `str(build_py.ROOT / "games/stub")`. Run: `.venv/bin/python -m pytest -q tests/test_build_py.py > /tmp/t.log 2>&1; echo exit $?` → fails.

- [ ] **Step 2: Implement in build.py**

```python
def build_root_for(game_dir, root=ROOT):
    """Outputs live beside the game when it is outside the kit, else in the kit's build/."""
    game_dir = Path(game_dir)
    try:
        game_dir.relative_to(root)
        return Path(root) / "build"
    except ValueError:
        return game_dir / "build"


def configure(preset, extra=(), build_dir=None):
    command = [cmake_tool("cmake"), "--preset", preset, "-DPython3_EXECUTABLE=" + sys.executable]
    if build_dir is not None:
        command += ["-B", str(build_dir)]
    subprocess.run(command + list(extra), cwd=ROOT, check=True)


def build(preset, targets, jobs, extra=(), build_dir=None):
    command = [cmake_tool("cmake"), "--build"]
    command += [str(build_dir)] if build_dir is not None else ["--preset", preset]
    command += ["--parallel", str(jobs), "--target"] + list(targets) + list(extra)
    subprocess.run(command, cwd=ROOT, check=True)
```

`archive_path(build_root, system=None)`, `ios_app_bundle(app_name, build_root)`, `publish_generated(build_root, translate)` (with `shutil.copy(ROOT / "runtime/x86.h", ...)` still from the kit), `run_translator(stage, game_dir, build_root)` (`--game str(game_dir)`, `--report build_root/"recomp/translate-report.json"`), `texture_pack(game_dir, build_root)`. In `parse_args`:

```python
    parser.add_argument("--game-dir", type=Path, default=ROOT / "games/stub",
                        help="Absolute directory holding the game.toml this build is for (default: the kit's stub game)")
    parser.add_argument("--build-root", type=Path, default=None,
                        help="Where outputs go (default: <game-dir>/build outside the kit, build/ inside it)")
    ...
    if not args.game_dir.is_absolute():
        parser.error("--game-dir must be absolute: %s" % args.game_dir)
    if not (args.game_dir / "game.toml").is_file():
        parser.error("No game config at %s/game.toml" % args.game_dir)
    args.build_root = args.build_root or build_root_for(args.game_dir)
```

In `main()`: `cfg = game_config.load(args.game_dir)`; the `developer_exe` checks use `cfg["developer_exe_path"]` and `cfg["listings_path"] / "functions.tsv"`; `BuildLock(args.build_root, ...)`; `build_dir = args.build_root / "cmake" / preset`; every `configure` gets `["-DRECOMP_GAME_DIR=%s" % args.game_dir, "-DPOP_BUILD_ROOT=%s" % args.build_root]` and `build_dir=build_dir`; every `build` gets `build_dir=build_dir`; the translation check reads `args.build_root / "recomp/gen/table.c"`.

`tools/recomp/buildlock.py`: `BuildLock(root, description)` already joins `LOCK_RELATIVE` onto `root`; callers now pass the build root's parent? No: change `LOCK_RELATIVE` to `os.path.join("recomp", ".lock")` and pass the build root (`<root>/build`) everywhere (`grep -rn 'BuildLock(\|buildlock_acquire' tools host | grep -v def`); `buildlock.sh` takes the same argument. Update `tools/recomp/tests/test_buildlock.py` accordingly.

- [ ] **Step 3: The other tools**

- `tools/test.py`: `--game-dir` (same validation, default stub), `build_root = build_py.build_root_for(args.game_dir)`; `mods()` and `gameplay()` take `game_dir, build_root`: fixture/case dirs under `build_root/"tests"` and `build_root/"gameplay"`, `pop_fixture`/`pop_smoke` under `build_root/"recomp"`, `POPM_CORE_MODS_DIR=build_root/"recomp/mods/core"`, the script from `game_dir/"smoke/native-options.script"` (Task 6 moves the scripts; until then `test.py` falls back to `ROOT/"tools/recomp/smoke/native-options.script"` when the game copy is absent), `configure`/`build` with `build_dir`. Add `--integration`: runs `host/tests/integration_tests.sh` with `RECOMP_GAME_DIR` and `POP_BUILD_ROOT` in the environment. `PORTABLE_TESTS` unchanged.
- `tools/setup.py`: `--game-dir` (default stub is meaningless here, so required); `original/` and `analysis/` under it; the Ghidra project name from `cfg["game"]["app_name"]`; the import of `cfg["developer_exe_path"]`.
- `tools/ios_logs.py`: `--game-dir` required; `--out` default `build_root/"ios-pull"`.
- `tools/recomp/translate.py`: drop `DEFAULT_GAME_DIR`; `--game` required; `configure()` uses `cfg["listings_path"]` and `cfg["developer_exe_path"]`; the module-level `configure(game_config.load(DEFAULT_GAME_DIR))` at line 58 becomes lazy (called from `main()` and from `tools/recomp/tests/test_translate*.py` via an explicit `translate.configure(game_config.load(os.environ["RECOMP_GAME_DIR"]))`; those tests skip with `unittest.SkipTest("RECOMP_GAME_DIR not set")` when the variable is absent, except `test_translate_config.py`, which points at `games/stub` and only checks configuration).
- `tools/recomp/oracle.py`: `--game-dir` argument; `DEFAULT_EXE`/`DEFAULT_DATA` computed in `main()`.
- `tools/recomp/build_core.py`: `--source` required (the game's `mods/CMakeLists.txt` passes `${RECOMP_GAME_DIR}/mods/core`; `finish_bundle.py` passes `--source` from a new `--game-dir` argument that `MacBundle.cmake` supplies as `${RECOMP_GAME_DIR}`).
- `host/tests/integration_tests.sh`: `GAME=${RECOMP_GAME_DIR:?}`, `BUILD=${POP_BUILD_ROOT:-$ROOT/build}`; every `build/recomp` becomes `$BUILD/recomp`; `tools/build.py --target plugins` gets `--game-dir "$GAME"`.

- [ ] **Step 4: Exercise the real game through the new path**

```bash
ln -s ../../original games/populous/original; ln -s ../../analysis games/populous/analysis   # temporary, Task 6 removes them
.venv/bin/python tools/build.py --game-dir $PWD/games/populous --jobs 8 > /tmp/b.log 2>&1; echo exit $?
.venv/bin/python tools/test.py --game-dir $PWD/games/populous > /tmp/t1.log 2>&1; echo exit $?
.venv/bin/python tools/test.py --game-dir $PWD/games/populous --native > /tmp/t2.log 2>&1; echo exit $?
.venv/bin/python tools/test.py --game-dir $PWD/games/populous --mods > /tmp/t3.log 2>&1; echo exit $?
.venv/bin/python tools/test.py --game-dir $PWD/games/populous --gameplay > /tmp/t4.log 2>&1; echo exit $?
RECOMP_IOS_TEAM=<team id> .venv/bin/python tools/build.py --game-dir $PWD/games/populous --target ios --no-install --jobs 8 > /tmp/b5.log 2>&1; echo exit $?
.venv/bin/python tools/build.py --stub --jobs 8 > /tmp/b6.log 2>&1; echo exit $?   # the stub game, default
```

Expected: every exit 0; the iOS bundle contains `game/`, the icon and `symbols.json` (`ls build/ios/Release/PopRecomp.app | head`). Then a build into an external root: `mkdir -p /tmp/ext && cp -R games/populous /tmp/ext/game && .venv/bin/python tools/build.py --game-dir /tmp/ext/game --target fixture --jobs 8 > /tmp/b7.log 2>&1; echo exit $?; ls /tmp/ext/game/build/cmake` → exit 0 and a `macos` directory there (`--target fixture` needs no translation; a full `app` needs `--regenerate`, which needs the listings, so `fixture` is the external smoke check).

- [ ] **Step 5: Commit**

```bash
.venv/bin/python tools/format.py --write
git add -A
git commit -m "tools: --game-dir replaces --game; outputs and the build lock follow the game's build root"
```

---

### Task 4: Kit documentation for external games

**Files:**
- Modify: `README.md` (build section: `--game-dir`, the stub, where games live), `docs/testing.md`, `docs/architecture.md` (the `games/` paragraph), `CONTRIBUTING.md` if present, `CHANGELOG.md`, `docs/superpowers/specs/2026-09-13-recomp-kit-design.md` (a note that games live in their own repos; section on `games/<id>` amended)

- [ ] **Step 1: Write the docs**

README: replace every `--game populous` / `games/populous` with the `--game-dir` form and add a "Games live in their own repositories" paragraph: a game repo holds `game.toml`, `globals.toml`, `core/`, `tests/`, `mods/`, `assets/`, `smoke/`, ignored `original/` and `analysis/`, and the kit as a submodule at `kit/`; `python3 kit/tools/build.py --game-dir "$PWD"` builds it; populous-recomp is the reference. CHANGELOG Unreleased: "- Builds take `--game-dir`: a game directory anywhere, with outputs under its own `build/`; the kit ships `games/stub` for game-free builds and CI. Populous moves to github.com/veritr1x/populous-recomp, which pulls the kit in as a submodule."

- [ ] **Step 2: Check links and commit**

Run: `.venv/bin/python tools/check_repo.py > /tmp/c.log 2>&1; echo exit $?`

```bash
git add -A
git commit -m "Docs: games live in their own repositories; --game-dir"
```

---

### Task 5: Stage the Populous files for the game repo

**Files:**
- Create: `<workspace>/populous-staging/` (outside both repos; deleted in Task 7)

- [ ] **Step 1: Copy with history-free `cp`**

```bash
S=<workspace>/populous-staging; rm -rf "$S"; mkdir -p "$S/tools/release" "$S/smoke" "$S/docs" "$S/tests"
cp -R games/populous/. "$S/"                       # game.toml, globals.toml, core/, tests/, mods/, assets/
rm -f "$S/original" "$S/analysis"                   # the temporary symlinks from Task 3
cp tools/recomp/smoke/*.script "$S/smoke/"
cp tools/recomp/release/README.txt "$S/tools/release/"
cp docs/DISPLAY.md docs/HD_TEXTURES.md docs/MODDING.md "$S/docs/"
ls -R "$S" | head -40
```

- [ ] **Step 2: Write the moved Populous assertions**

`$S/tests/test_game_config.py`:

```python
"""Populous's game.toml renders the values the kit's hooks expect."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
KIT = ROOT / "kit"


def load_module(name):
    spec = importlib.util.spec_from_file_location(name, KIT / "tools" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


game_config = load_module("game_config")
gen_game_config = load_module("gen_game_config")


class PopulousConfigTests(unittest.TestCase):
    def setUp(self):
        self.cfg = game_config.load(ROOT)
        self.header = gen_game_config.render_header(self.cfg)

    def test_identity(self):
        self.assertEqual(self.cfg["game"]["id"], "populous")
        self.assertEqual(self.cfg["game"]["executable"], "D3DPopTB.exe")
        self.assertEqual(self.cfg["game"]["entry_point"], 0x0055D6C0)
        self.assertIn('#define RECOMP_GUEST_ROOT "C:\\\\Populous"', self.header)
        self.assertEqual(self.cfg["developer_exe_path"], ROOT / "original/gog/D3DPopTB.exe")

    def test_hooks_and_globals(self):
        self.assertIn("#define RECOMP_HOOK_FRAME_CLOCK_BEGIN 0x004a45a3u", self.header)
        self.assertIn("#define RECOMP_HOOK_CURSOR_SURFACE_PTRS {0x005d5718u, 0x005d571cu}", self.header)
        self.assertIn("#define RECOMP_HOOK_MOUSE_VTABLE 0x00591b6cu", self.header)
        self.assertIn("#define RECOMP_HOOK_CAMERA 0x0074a350u", self.header)
        self.assertIn("#define RECOMP_GLOBAL_SIMULATION_TURN_ADDR 0x0089d188u", self.header)
        self.assertIn("#define RECOMP_GLOBAL_ENTITY_BASE_STRIDE 179u", self.header)

    def test_bundle_exclusions(self):
        self.assertIn("Fmv", self.cfg["bundle"]["exclude"])
        self.assertIn("*.dll", self.cfg["bundle"]["exclude"])


if __name__ == "__main__":
    unittest.main()
```

Remove the equivalent Populous assertions from the kit's `tests/test_game_config.py` and `tests/test_stage_game_files.py` (`test_populous_config_lists_exclusions` becomes a temp-dir test with its own exclude list), pointing what remains at `games/stub`. Do the same for `tools/recomp/tests/test_translate_config.py` (stub). Run `.venv/bin/python tools/test.py > /tmp/t.log 2>&1; echo exit $?` → 0.

- [ ] **Step 3: Commit the kit-side test changes**

```bash
git add -A
git commit -m "Tests: kit fixtures use games/stub; Populous assertions staged for populous-recomp"
```

---

### Task 6: Remove Populous from the kit

**Files:**
- Delete: `games/populous/`, `tools/recomp/smoke/*.script`, `tools/recomp/release/`, `docs/DISPLAY.md`, `docs/HD_TEXTURES.md`, `docs/MODDING.md`, the `original` and `analysis` symlinks
- Modify: every reference found by `grep -rn 'games/populous\|tools/recomp/smoke\|recomp/release\|DISPLAY.md\|HD_TEXTURES.md\|MODDING.md' --exclude-dir=build --exclude-dir=.git --exclude-dir=docs/superpowers .`; `tools/check_repo.py` link list; `.github/workflows/checks.yml` (nothing should change, verify); `host/smoke_main.cpp:207` comment

- [ ] **Step 1: Delete and fix references**

```bash
git rm -r -q games/populous tools/recomp/smoke tools/recomp/release docs/DISPLAY.md docs/HD_TEXTURES.md docs/MODDING.md
git rm -q original analysis
grep -rn 'games/populous\|tools/recomp/smoke\|recomp/release\|DISPLAY.md\|HD_TEXTURES.md\|MODDING.md' --exclude-dir=build --exclude-dir=.git --exclude-dir=.venv . | grep -v 'docs/superpowers'
```

Fix each hit: docs point at populous-recomp for Populous matters; `tools/test.py`'s fallback script path goes (the game dir must have `smoke/`); `.gitignore` keeps `/original/` and `/analysis/` lines (harmless).

- [ ] **Step 2: Full verification on the stub and on the staged game**

```bash
.venv/bin/python tools/test.py > /tmp/t.log 2>&1; echo exit $?
.venv/bin/python tools/build.py --stub --jobs 8 > /tmp/b.log 2>&1; echo exit $?
.venv/bin/python tools/test.py --native > /tmp/t2.log 2>&1; echo exit $?         # stub: game-labelled suites skip
RECOMP_IOS_TEAM= .venv/bin/python tools/build.py --stub --target ios --jobs 8 > /tmp/b3.log 2>&1; echo exit $?
S=<workspace>/populous-staging; ln -sfn "$PWD/../populous-recomp-checkout/original" "$S/original"; ln -sfn "$PWD/../populous-recomp/analysis" "$S/analysis"
.venv/bin/python tools/build.py --game-dir "$S" --regenerate --jobs 8 > /tmp/b4.log 2>&1; echo exit $?
.venv/bin/python tools/test.py --game-dir "$S" --native > /tmp/t5.log 2>&1; echo exit $?
.venv/bin/python tools/test.py --game-dir "$S" --gameplay > /tmp/t6.log 2>&1; echo exit $?
RECOMP_IOS_TEAM=<team id> .venv/bin/python tools/build.py --game-dir "$S" --target ios --jobs 8 > /tmp/b7.log 2>&1; echo exit $?
```

Expected: all 0; the last installs and launches Populous on the iPad from the staged game directory with outputs under `$S/build`. `tools/check_game_literals.py` still passes (`.venv/bin/python tools/check_game_literals.py; echo exit $?`).

- [ ] **Step 3: Commit, push, CI, tag**

```bash
.venv/bin/python tools/format.py --write
git add -A
git commit -m "Populous leaves the kit: the game folder, smoke scripts, release notes and game docs move to populous-recomp"
git push -u origin game-repo-split
gh run watch $(gh run list --branch game-repo-split --limit 1 --json databaseId --jq '.[0].databaseId')
```

When CI is green, use superpowers:finishing-a-development-branch to merge into `main`, then `git tag -a m1.1 -m "M1.1: games build from their own repositories" && git push origin m1.1`.

---

### Task 7: The thin populous-recomp

**Files (in `<workspace>/populous-recomp-checkout`):**
- Create: `.gitmodules` (via `git submodule add`), `kit/` submodule, `tools/build.py`, `tools/test.py`, `tools/setup.py`, `tools/ios_logs.py`, `tests/test_game_config.py` (from staging), `.github/workflows/checks.yml`, `AGENTS.md`, `CONTRIBUTING.md`, `README.md`, `docs/testing.md`
- Delete: `src/`, `third_party/`, `cmake/`, `translation/`, `CMakeLists.txt`, `CMakePresets.json`, `Makefile`, `requirements-dev.txt`, `.github/workflows/release.yml`, old `tools/*`, old `tests/*`, old `mods/`, old `docs/*` (replaced by the staged copies), `assets/` (replaced)

- [ ] **Step 1: Tag the old tree and branch**

```bash
cd <workspace>/populous-recomp-checkout
git status --short          # expect one untracked line at most; stop if tracked files are modified
git tag -a legacy-macos-source -m "The single-repo macOS source tree before the kit split"
git push origin legacy-macos-source
git checkout -b thin-kit
```

- [ ] **Step 2: Replace the tree**

```bash
git rm -r -q src third_party cmake translation CMakeLists.txt CMakePresets.json Makefile requirements-dev.txt .github/workflows/release.yml tools tests mods docs assets
S=<workspace>/populous-staging
rm -f "$S/original" "$S/analysis"; rm -rf "$S/build"
cp -R "$S/." .
git submodule add https://github.com/veritr1x/recomp-kit.git kit
git -C kit checkout m1.1
ls
```

- [ ] **Step 3: Wrappers**

`tools/build.py`:

```python
#!/usr/bin/env python3
"""Build Populous with the kit in kit/. Every option is the kit's: see kit/tools/build.py --help."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KIT = ROOT / "kit"
if not (KIT / "tools/build.py").is_file():
    sys.exit("The kit submodule is missing: run `git submodule update --init`")
sys.exit(subprocess.call([sys.executable, str(KIT / "tools/build.py"), "--game-dir", str(ROOT)] + sys.argv[1:],
                         cwd=ROOT))
```

`tools/test.py`, `tools/setup.py`, `tools/ios_logs.py`: identical with the script name changed and the docstring naming it. `chmod +x tools/*.py`.

- [ ] **Step 4: Ignore rules, docs, CI**

`.gitignore`: keep the existing list; ensure `/original/`, `/analysis/`, `/build/`, `/.venv/` are present.

`README.md` (rewrite the top): what the repo is (Populous: The Beginning on macOS and iPad, built with recomp-kit), the clone line with `--recurse-submodules`, the venv from `kit/requirements-dev.txt`, `tools/setup.py` for the game, `tools/build.py` for macOS, `RECOMP_IOS_TEAM=<team> tools/build.py --target ios` for the iPad, `tools/test.py` variants, and pointers to `docs/`. State that the kit is private at the moment, so the submodule needs access to it.

`CONTRIBUTING.md`: the old prerequisites minus `translation/` (it is regenerated, `tools/build.py --regenerate`), the venv from `kit/requirements-dev.txt`, the hash paragraph kept. `AGENTS.md`: the old rules, with `src/recomp/platform/os.h` becoming `kit/platform/os.h` and "edit the kit in its own repository; this repo holds only Populous". `docs/testing.md`: the suites table with the wrapper commands. `CHANGELOG.md`: a new entry "Thin repository: the runtime moved to recomp-kit (submodule at kit/); this repository holds Populous's config, mods, artwork, smoke scripts and docs. The previous tree is tag legacy-macos-source."

`.github/workflows/checks.yml`:

```yaml
name: Checks
on:
  push:
    branches: ["**"]
  pull_request:
  workflow_dispatch:
permissions:
  contents: read
jobs:
  # The kit is a private submodule for now; checkout needs KIT_TOKEN (a PAT
  # with read access to veritr1x/recomp-kit) in this repository's secrets.
  # Until it exists this workflow fails at checkout, which is the expected state.
  configure:
    strategy:
      fail-fast: false
      matrix:
        include:
          - os: macos-15
          - os: ubuntu-24.04
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@v7.0.1
        with:
          submodules: recursive
          token: ${{ secrets.KIT_TOKEN }}
      - name: Contributor tools
        run: |
          python3 -m venv .venv
          .venv/bin/python -m pip install -r kit/requirements-dev.txt
      - name: Portable tests (kit suites plus this game's config)
        run: |
          .venv/bin/python tools/test.py
          .venv/bin/python -m pytest -q tests
      - name: Link-only configure of this game through the kit
        run: .venv/bin/python tools/build.py --stub
      - name: iOS stub compile (no signing)
        if: runner.os == 'macOS'
        run: .venv/bin/python tools/build.py --stub --target ios
```

(`tools/build.py --stub` with a game that has no `original/` must succeed: the stub preset needs no translation; confirm in Task 6 step 2's stub run.)

- [ ] **Step 5: Build everything from the game repo**

```bash
python3 -m venv .venv && .venv/bin/python -m pip install -r kit/requirements-dev.txt > /tmp/pip.log 2>&1; echo exit $?
ls original/gog/D3DPopTB.exe; ln -sfn ../populous-recomp/analysis analysis   # the developer's listings
.venv/bin/python tools/build.py --regenerate --jobs 8 > build-regen.log 2>&1; echo exit $?
.venv/bin/python tools/test.py > /tmp/t1.log 2>&1; echo exit $?
.venv/bin/python -m pytest -q tests > /tmp/t2.log 2>&1; echo exit $?
.venv/bin/python tools/test.py --native > /tmp/t3.log 2>&1; echo exit $?
.venv/bin/python tools/test.py --mods > /tmp/t4.log 2>&1; echo exit $?
.venv/bin/python tools/test.py --gameplay > /tmp/t5.log 2>&1; echo exit $?
RECOMP_IOS_TEAM=<team id> .venv/bin/python tools/build.py --target ios --jobs 8 > build-ios.log 2>&1; echo exit $?
```

Expected: all 0; outputs under `populous-recomp-checkout/build/`; the iPad shows Populous. Ask the user to play a minute (taps, keypad strip, F10 settings persist). Move the two `.log` files under `build/` afterwards.

- [ ] **Step 6: Commit, push, tag**

```bash
git add -A
git commit -m "Thin repository: Populous on top of recomp-kit (submodule at kit/)

The runtime, translator and hosts live in recomp-kit; this repository keeps
the game's config, curated globals, headers, mods, artwork, smoke scripts,
release notes and docs, and drives the kit through tools/*.py wrappers.
The previous single-repo tree is tag legacy-macos-source."
git push -u origin thin-kit
```

Then superpowers:finishing-a-development-branch (merge into `main`, push), `git tag -a kit-1 -m "First build on recomp-kit m1.1" && git push origin kit-1`, and `rm -rf <workspace>/populous-staging`. Finally, in the kit, remove the temporary symlinks if any remain (`git status --short` clean) and update the memory note about where Populous lives.
